// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Use of this source code is governed by MIT license.
//
// Emscripten environment implementation using Node.js fs APIs.
// Provides filesystem access for LevelDB running in Emscripten/Node.js.

#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <pthread.h>
#include <string.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <deque>
#include <set>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/val.h>
#include <leveldb/env.h>
#include <leveldb/slice.h>
#include "port/port.h"
#include "util/mutexlock.h"
#include "emldb_utils.h"

namespace leveldb {

namespace {

// Thread-local fs module reference (set from JavaScript)
// Each thread needs its own emscripten::val since they cannot be shared across threads
thread_local emscripten::val* tls_fs = nullptr;

// Global flag to track if fs has been initialized
static std::atomic<bool> g_fs_initialized(false);

// How many bytes EmWritableFile accumulates before handing them to Node.
//
// LevelDB writes each log record as two Appends - a 7 byte header and the
// payload - and each table block as two more, the contents and its trailer.
// One fs.writeSync per Append costs roughly 3us, which dominated fillseq and
// made single puts four times slower than batched ones. Buffering merges them
// into one write per threshold (or per Flush, see EmWritableFile::Flush).
//
// The tradeoff is the unflushed tail: with buffering on, a crashed process
// loses up to this many bytes of WAL, where upstream leveldb would only lose
// them to a machine crash because the data already sits in the OS page cache.
// Set 0 to opt out and keep the upstream guarantee exactly.
const size_t kDefaultFileWriteBuffer = 64 * 1024;

// A caller that never reaches Flush()/Sync()/Close() must not be able to grow
// the buffer without bound. Only reachable with buffering on, since otherwise
// Append never holds more than one record's worth of bytes.
const size_t kMaxFileWriteBuffer = 4 * 1024 * 1024;

static std::atomic<size_t> g_file_write_buffer(kDefaultFileWriteBuffer);

// Helper structure for StartThread
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};

EM_JS(emscripten::EM_VAL, requireFS, (), {
  return Emval.toHandle(require("fs"));
});

// Creates the sidecar guard file that stands in for the flock() Node does not
// expose, recovering it when the previous owner died without unlocking. A
// killed test run or a crashed process used to leave the guard behind and
// force a manual delete before the database could be reopened.
//
// Returns 0 on success, 1 on failure with a message written to errorOut. The
// message is passed out rather than thrown because a JS exception raised
// inside an EM_JS body has to unwind through wasm frames to reach a C++
// catch, which is not something this file should depend on.
EM_JS(int, emJsAcquireGuard, (const char *guardName, char *errorOut, size_t errorCap), {
  var fs = require('fs');
  var guard = UTF8ToString(guardName);
  var host = require('os').hostname();
  var owner = 'pid=' + process.pid + '\nhost=' + host +
              '\ntime=' + new Date().toISOString() + '\n';

  // A guard file appears before its owner text is written, so an ownerless one
  // is usually a concurrent acquisition mid-write rather than a broken lock.
  // Re-reading covers that in a few hundred microseconds; a writer that died
  // inside the window leaves a guard nobody can ever take, which is the manual
  // cleanup this recovery exists to remove, so the retries are bounded.
  //
  // The retries spin rather than sleep on purpose: the window being covered is
  // microseconds wide, and a timer-based wait costs ~15ms per iteration on
  // Windows regardless of the timeout asked for.
  var OWNERLESS_SPINS = 200;
  var MAX_ATTEMPTS = 400;

  function create() {
    var fd = fs.openSync(guard, 'wx');
    try {
      fs.writeSync(fd, Buffer.from(owner));
      fs.closeSync(fd);
    } catch (error) {
      // Do not leave an ownerless guard behind if the write failed.
      try { fs.closeSync(fd); } catch (ignored) { }
      try { fs.unlinkSync(guard); } catch (ignored) { }
      throw error;
    }
  }

  function discard(reason) {
    process.emitWarning('recovering lock ' + guard + ', ' + reason,
      { code: 'EMLDB_STALE_LOCK' });
    try {
      fs.unlinkSync(guard);
    } catch (error) {
      if (error.code !== 'ENOENT') throw error;
    }
  }

  try {
    for (var attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
      try {
        create();
        return 0;
      } catch (error) {
        if (error.code !== 'EEXIST') throw error;
      }

      var raw;
      try {
        raw = fs.readFileSync(guard, 'utf8');
      } catch (error) {
        if (error.code === 'ENOENT') continue;  // released before we read it
        throw error;
      }

      var pidMatch = raw.match(/^pid=([0-9]+)/m);
      if (!pidMatch) {
        if (attempt < OWNERLESS_SPINS) continue;
        discard('which records no owner');
        continue;
      }

      var pid = parseInt(pidMatch[1], 10);
      // Guards written before this format recorded the host have only the pid
      // line; treat those as local, which is what the code that wrote them
      // assumed, so upgrading does not strand an existing lock.
      var hostMatch = raw.match(/^host=(.*)/m);
      var ownerHost = hostMatch ? hostMatch[1].trim() : host;

      if (ownerHost !== host) {
        stringToUTF8('database is locked by process ' + pid + ' on ' + ownerHost +
          ', whose liveness cannot be checked from ' + host +
          '; delete this file only once that process is known to be gone',
          errorOut, errorCap);
        return 1;
      }

      // Pids are unique among running processes, so our own pid on a guard the
      // caller holds no in-process lock for means the previous owner is gone.
      var live = pid !== process.pid;
      if (live) {
        try {
          process.kill(pid, 0);
        } catch (error) {
          if (error.code === 'ESRCH') live = false;
          else if (error.code !== 'EPERM') throw error;  // EPERM means it exists
        }
      }

      if (live) {
        stringToUTF8('database is locked by live process ' + pid + ' on ' + host +
          '; close that database, or delete this file if that process is gone',
          errorOut, errorCap);
        return 1;
      }

      discard('left behind by process ' + pid + ' on ' + host +
        ', which is no longer running');
    }
    stringToUTF8('gave up acquiring the database lock after ' + MAX_ATTEMPTS +
      ' attempts', errorOut, errorCap);
    return 1;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), errorOut, errorCap);
    return 1;
  }
});

// Removes the guard file, but only while it still names this process. A guard
// another process recovered as stale belongs to that process now, and deleting
// it would let two databases open on the same directory.
//
// Returns 0 on success, 1 on failure with a message written to errorOut.
EM_JS(int, emJsReleaseGuard, (const char *guardName, int pid, char *errorOut, size_t errorCap), {
  var fs = require('fs');
  var guard = UTF8ToString(guardName);

  try {
    var raw = fs.readFileSync(guard, 'utf8');
    var pidMatch = raw.match(/^pid=([0-9]+)/m);
    if (pidMatch && parseInt(pidMatch[1], 10) !== pid) return 0;
    fs.unlinkSync(guard);
    return 0;
  } catch (error) {
    // Nothing left to release, or nothing that is still ours.
    if (error.code === 'ENOENT') return 0;
    stringToUTF8(String((error && error.message) || error), errorOut, errorCap);
    return 1;
  }
});

// Helper function to get or initialize the fs module for current thread
inline emscripten::val GetFS() {
  if (tls_fs == nullptr) {
    // Each thread gets fs module from JavaScript require('fs')
    tls_fs = new emscripten::val(emscripten::val::take_ownership(requireFS()));
  }
  return *tls_fs;
}

// Check if fs is available
inline bool HasFS() {
  return g_fs_initialized.load(std::memory_order_acquire);
}

Status FsError(const std::string& path, const std::string& operation,
               const emscripten::val& error) {
  std::string message = operation + " failed";
  std::string code;
  try {
    emscripten::val detail = error["message"];
    if (!detail.isUndefined() && !detail.isNull()) {
      message += ": " + detail.as<std::string>();
    }
    emscripten::val errorCode = error["code"];
    if (!errorCode.isUndefined() && !errorCode.isNull()) {
      code = errorCode.as<std::string>();
    }
  } catch (...) { }
  if (code == "ENOENT") return Status::NotFound(path, message);
  return Status::IOError(path, message);
}

}  // anonymous namespace

// Sequential file implementation using Node.js fs.open/read/close
class EmSequentialFile : public SequentialFile {
public:
  EmSequentialFile(const std::string& path, i32 fd)
      : m_path(path), m_fd(fd) {}

  EmSequentialFile(const EmSequentialFile&) = delete;
  EmSequentialFile& operator=(const EmSequentialFile&) = delete;

  virtual ~EmSequentialFile() noexcept {
    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) { }
    }
  }

  virtual Status Read(size_t n, Slice* result, char* scratch) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    try {
      emscripten::val fs = GetFS();
      emscripten::val target = emscripten::val(
        emscripten::typed_memory_view(n, reinterpret_cast<unsigned char*>(scratch)));
      size_t bytesRead = fs.call<size_t>(
        "readSync", m_fd, target, 0, n, emscripten::val::null());
      *result = Slice(scratch, bytesRead);
      return Status::OK();
    } catch (const emscripten::val& error) {
      *result = Slice();
      return FsError(m_path, "read", error);
    }
  }

  virtual Status Skip(u64 n) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    // Node does not expose lseek. Advance in bounded chunks instead of
    // allocating a buffer proportional to an arbitrary skip distance.
    const size_t kChunkSize = 64 * 1024;
    try {
      emscripten::val fs = GetFS();
      emscripten::val buffer = emscripten::val::global("Buffer").call<emscripten::val>(
        "allocUnsafe", kChunkSize);
      while (n > 0) {
        size_t chunk = n < kChunkSize ? static_cast<size_t>(n) : kChunkSize;
        size_t bytesRead = fs.call<size_t>(
          "readSync", m_fd, buffer, 0, chunk, emscripten::val::null());
        if (bytesRead == 0) break;
        n -= bytesRead;
      }
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(m_path, "skip", error);
    }
  }

private:
  std::string m_path;
  i32 m_fd;
};

// Random access file implementation using fd and pread
class EmRandomAccessFile : public RandomAccessFile {
public:
  EmRandomAccessFile(const std::string& path, i32 fd)
      : m_path(path), m_fd(fd) {}

  EmRandomAccessFile(const EmRandomAccessFile&) = delete;
  EmRandomAccessFile& operator=(const EmRandomAccessFile&) = delete;

  virtual ~EmRandomAccessFile() noexcept {
    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) { }
    }
  }

  bool IsValid() const { return m_fd != -1; }

  virtual Status Read(u64 offset, size_t n, Slice* result, char* scratch) const override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    try {
      emscripten::val fs = GetFS();
      emscripten::val target = emscripten::val(
        emscripten::typed_memory_view(n, reinterpret_cast<unsigned char*>(scratch)));
      size_t bytesRead = fs.call<size_t>(
        "readSync", m_fd, target, 0, n, static_cast<double>(offset));
      *result = Slice(scratch, bytesRead);
      return Status::OK();
    } catch (const emscripten::val& error) {
      *result = Slice();
      return FsError(m_path, "random read", error);
    }
  }

private:
  std::string m_path;
  i32 m_fd;
};

// Writable file implementation using Node.js fs.open/write/close.
//
// Appends land in an in-process buffer that is handed to Node in one
// fs.writeSync per kDefaultFileWriteBuffer bytes rather than one per Append.
// See the constants above for the cost and the durability tradeoff.
//
// Every writer owns its file, and LevelDB only ever touches one file from one
// thread, so the buffer needs no locking; the threshold is read once at
// construction from the atomic global.
class EmWritableFile : public WritableFile {
public:
  EmWritableFile(const std::string& path, i32 fd)
      : m_path(path),
        m_fd(fd),
        m_threshold(g_file_write_buffer.load(std::memory_order_relaxed)) {}

  EmWritableFile(const EmWritableFile&) = delete;
  EmWritableFile& operator=(const EmWritableFile&) = delete;

  virtual ~EmWritableFile() noexcept {
    if (m_fd != -1) {
      try {
        // Best effort: a destructor cannot report a failure, but dropping the
        // buffer would silently lose acknowledged writes.
        FlushBuffer();
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) { }
    }
  }

  bool IsValid() const { return m_fd != -1; }

  virtual Status Append(const Slice& data) override {
    if (m_fd == -1) {
      return Status::IOError(m_path, "file not open");
    }

    m_buffer.append(data.data(), data.size());

    if (m_buffer.size() >= kMaxFileWriteBuffer ||
        (m_threshold != 0 && m_buffer.size() >= m_threshold)) {
      return FlushBuffer();
    }
    return Status::OK();
  }

  virtual Status Close() override {
    if (m_fd == -1) {
      return Status::OK();
    }

    // A failed flush must not close the fd: the caller has to be able to retry,
    // and a closed handle would make the lost bytes unrecoverable.
    Status status = FlushBuffer();
    if (!status.ok()) {
      return status;
    }

    try {
      emscripten::val fs = GetFS();
      fs.call<void>("closeSync", m_fd);
      m_fd = -1;
    } catch (const emscripten::val& error) {
      return FsError(m_path, "close", error);
    }
    return Status::OK();
  }

  virtual Status Flush() override {
    // LevelDB calls this after every physical record, so flushing here would
    // defeat the buffer entirely. With buffering disabled (threshold 0) it is
    // the only thing that pushes a record out, which is what keeps that mode
    // byte-for-byte identical to upstream: there each Append already reached
    // the OS and Flush() was a no-op.
    if (m_threshold == 0) {
      return FlushBuffer();
    }
    return Status::OK();
  }

  virtual Status Sync() override {
    if (m_fd == -1) {
      return Status::IOError(m_path, "file not open");
    }

    Status status = FlushBuffer();
    if (!status.ok()) {
      return status;
    }

    try {
      emscripten::val fs = GetFS();
      fs.call<void>("fsyncSync", m_fd);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(m_path, "fsync", error);
    }
  }

private:
  // Writes the buffer out in one fs.writeSync, retrying on a short write. The
  // already-written prefix is dropped on failure so a later retry cannot
  // duplicate bytes into the log.
  Status FlushBuffer() {
    if (m_buffer.empty()) {
      return Status::OK();
    }
    if (m_fd == -1) {
      return Status::IOError(m_path, "file not open");
    }

    size_t offset = 0;
    try {
      emscripten::val fs = GetFS();
      while (offset < m_buffer.size()) {
        const size_t remaining = m_buffer.size() - offset;
        emscripten::val buffer = emscripten::val(
          emscripten::typed_memory_view(remaining,
            reinterpret_cast<const unsigned char*>(m_buffer.data() + offset)));
        size_t bytesWritten = fs.call<size_t>("writeSync", m_fd, buffer);
        if (bytesWritten == 0 || bytesWritten > remaining) {
          m_buffer.erase(0, offset);
          return Status::IOError(m_path, "write made no progress");
        }
        offset += bytesWritten;
      }
    } catch (const emscripten::val& error) {
      m_buffer.erase(0, offset);
      return FsError(m_path, "write", error);
    }

    m_buffer.clear();
    return Status::OK();
  }

  std::string m_path;
  i32 m_fd;
  size_t m_threshold;
  std::string m_buffer;
};

// File lock backed by an atomically-created cross-process guard file.
class EmFileLock : public FileLock {
public:
  std::string name_;        // the LOCK path, used to drop the in-process entry
  std::string guard_name_;  // the sidecar guard file this lock created
};

// Lock table to prevent multiple locks on same file
class EmLockTable {
private:
  port::Mutex mu_;
  std::set<std::string> locked_files_;

public:
  bool Insert(const std::string& fname) {
    MutexLock l(&mu_);
    return locked_files_.insert(fname).second;
  }

  void Remove(const std::string& fname) {
    MutexLock l(&mu_);
    locked_files_.erase(fname);
  }
};

// Logger implementation using fd
class EmLogger : public Logger {
public:
  EmLogger(i32 fd) : m_fd(fd) {}

  virtual ~EmLogger() {
    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) { }
    }
  }

  virtual void Logv(const char* format, va_list ap) override {
    va_list copy;
    va_copy(copy, ap);
    int required = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (required < 0 || m_fd == -1) return;

    std::vector<char> buffer(static_cast<size_t>(required) + 1);
    vsnprintf(buffer.data(), buffer.size(), format, ap);

    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        size_t len = static_cast<size_t>(required);
        emscripten::val buf = emscripten::val(
          emscripten::typed_memory_view(len,
            reinterpret_cast<const unsigned char*>(buffer.data())));
        fs.call<size_t>("writeSync", m_fd, buf);
      } catch (...) { }
    }
  }

private:
  i32 m_fd;
};

// Main Emscripten environment implementation
class EmscriptenEnv: public Env {
public:
  EmscriptenEnv();

  virtual ~EmscriptenEnv() {
    // Shut down background thread
    {
      MutexLock lock(&mu_);
      shutdown_ = true;
      bgsignal_.Signal();
    }

    if (bgthread_started_) {
      pthread_join(bgthread_, nullptr);
    }
  }

  virtual Status NewSequentialFile(const std::string& fname, SequentialFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("r"));
      *result = new EmSequentialFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(fname, "open sequential file", error);
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("r"));
      *result = new EmRandomAccessFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(fname, "open random access file", error);
    }
  }

  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("w"));
      *result = new EmWritableFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(fname, "create writable file", error);
    }
  }

  virtual Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("a"));
      *result = new EmWritableFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(fname, "open appendable file", error);
    }
  }

  virtual bool FileExists(const std::string& fname) override {
    if (!HasFS()) {
      return false;
    }
    try {
      emscripten::val fs = GetFS();
      return fs.call<bool>("existsSync", fname);
    } catch (...) {
      return false;
    }
  }

  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      emscripten::val files = fs.call<emscripten::val>("readdirSync", dir);
      i32 length = files["length"].as<i32>();
      for (i32 i = 0; i < length; ++i) {
        result->push_back(files[i].as<std::string>());
      }
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(dir, "read directory", error);
    }
  }

  virtual Status DeleteFile(const std::string& fname) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      if (!fs.call<bool>("existsSync", fname))
        return Status::NotFound(fname);
      if (fs.call<emscripten::val>("statSync", fname).call<bool>("isDirectory"))
        return Status::IOError(fname, "expected a file, found a directory");
      fs.call<void>("unlinkSync", fname);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(fname, "delete file", error);
    }
  }

  virtual Status CreateDir(const std::string& dirname) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      if (fs.call<bool>("existsSync", dirname)) {
        if (!fs.call<emscripten::val>("statSync", dirname).call<bool>("isDirectory"))
          return Status::IOError(dirname, "path exists and is not a directory");
        return Status::OK();
      }
      emscripten::val options = emscripten::val::object();
      options.set("recursive", false);
      fs.call<void>("mkdirSync", dirname, options);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(dirname, "create directory", error);
    }
  }

  virtual Status DeleteDir(const std::string& dirname) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      if (!fs.call<bool>("existsSync", dirname))
        return Status::NotFound(dirname);
      if (!fs.call<emscripten::val>("statSync", dirname).call<bool>("isDirectory"))
        return Status::IOError(dirname, "expected a directory, found a file");
      fs.call<void>("rmdirSync", dirname);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(dirname, "delete directory", error);
    }
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) override {
    *file_size = 0;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    i32 fdNum = -1;
    try {
      emscripten::val fs = GetFS();
      fdNum = fs.call<i32>("openSync", fname, std::string("r"));
      emscripten::val stats = fs.call<emscripten::val>("fstatSync", fdNum);
      *file_size = stats["size"].as<u64>();
      fs.call<void>("closeSync", fdNum);
      return Status::OK();
    } catch (const emscripten::val& error) {
      if (fdNum != -1) {
        try { GetFS().call<void>("closeSync", fdNum); } catch (...) { }
      }
      return FsError(fname, "get file size", error);
    }
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      if (!fs.call<bool>("existsSync", src))
        return Status::NotFound(src);
      fs.call<void>("renameSync", src, target);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(src, "rename to " + target, error);
    }
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Check if already locked by this process
    if (!m_locks.Insert(fname)) {
      return Status::IOError("lock " + fname, "already held by process");
    }

    // Node core does not expose flock, so the cross-process guard is a sidecar
    // file created with O_EXCL. emJsAcquireGuard also clears a guard whose
    // owner died without unlocking, so a killed process no longer leaves the
    // database needing a manual delete before the next open.
    const std::string guardName = fname + ".emldb-lock";
    char message[512] = {};
    if (emJsAcquireGuard(guardName.c_str(), message, sizeof(message)) != 0) {
      m_locks.Remove(fname);
      return Status::IOError(guardName, message);
    }

    EmFileLock* my_lock = new EmFileLock;
    my_lock->name_ = fname;
    my_lock->guard_name_ = guardName;
    *lock = my_lock;
    return Status::OK();
  }

  virtual Status UnlockFile(FileLock* lock) override {
    EmFileLock* my_lock = reinterpret_cast<EmFileLock*>(lock);
    Status status = Status::OK();
    if (HasFS()) {
      const i32 pid = emscripten::val::global("process")["pid"].as<i32>();
      char message[512] = {};
      if (emJsReleaseGuard(my_lock->guard_name_.c_str(), pid,
                           message, sizeof(message)) != 0) {
        status = Status::IOError(my_lock->guard_name_, message);
      }
    }
    m_locks.Remove(my_lock->name_);
    delete my_lock;
    return status;
  }

  virtual void Schedule(void (*function)(void*), void* arg) override {
    mu_.Lock();

    // Start background thread if necessary
    if (!bgthread_started_) {
      bgthread_started_ = true;
      if (pthread_create(&bgthread_, nullptr, &EmscriptenEnv::BGThreadWrapper, this) != 0) {
        bgthread_started_ = false;
        mu_.Unlock();
        throw std::runtime_error("unable to start LevelDB background thread");
      }
    }

    // Add to task queue
    queue_.push_back(BGItem());
    queue_.back().function = function;
    queue_.back().arg = arg;

    bgsignal_.Signal();
    mu_.Unlock();
  }

  virtual void StartThread(void (*function)(void* arg), void* arg) override {
    StartThreadState* state = new StartThreadState;
    state->user_function = function;
    state->arg = arg;

    pthread_t thread;
    if (pthread_create(&thread, nullptr, &StartThreadWrapper, state) != 0) {
      delete state;
      throw std::runtime_error("unable to start LevelDB worker thread");
    }

    // Detach the thread so it can run independently
    pthread_detach(thread);
  }

  virtual Status GetTestDirectory(std::string* result) override {
    *result = "/tmp/leveldbtest";
    return CreateDir(*result);
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("w"));
      *result = new EmLogger(fdNum);
      return Status::OK();
    } catch (const emscripten::val& error) {
      return FsError(fname, "create logger", error);
    }
  }

  virtual uint64_t NowMicros() override {
    emscripten::val performance = emscripten::val::global("performance");
    double millis;
    if (!performance.isUndefined() && !performance.isNull()) {
      millis = performance["timeOrigin"].as<double>() + performance.call<double>("now");
    } else {
      millis = emscripten::val::global("Date").call<double>("now");
    }
    return static_cast<u64>(millis * 1000);
  }

  virtual void SleepForMicroseconds(int micros) override {
    if (micros > 0) emscripten_thread_sleep(static_cast<double>(micros) / 1000.0);
  }

private:
  // Background thread implementation
  void BGThread() {
    while (true) {
      // Wait until there is an item that is ready to run
      mu_.Lock();

      while (queue_.empty() && !shutdown_) {
        bgsignal_.Wait();
      }

      if (shutdown_) {
        mu_.Unlock();
        break;
      }

      void (*function)(void*) = queue_.front().function;
      void* arg = queue_.front().arg;
      queue_.pop_front();

      mu_.Unlock();
      (*function)(arg);
    }
  }

  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<EmscriptenEnv*>(arg)->BGThread();
    return nullptr;
  }

  static void* StartThreadWrapper(void* arg) {
    StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
    state->user_function(state->arg);
    delete state;
    return nullptr;
  }

  EmLockTable m_locks;

  // Background thread state
  port::Mutex mu_;
  port::CondVar bgsignal_;
  pthread_t bgthread_;
  bool bgthread_started_;
  bool shutdown_;

  // Entry per Schedule() call
  struct BGItem {
    void* arg;
    void (*function)(void*);
  };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;
};

}  // namespace leveldb

// Export the default environment
namespace leveldb {

EmscriptenEnv::EmscriptenEnv()
    : bgsignal_(&mu_), bgthread_started_(false), shutdown_(false) {
}

Env* Env::Default() {
  // C++11 guarantees thread-safe initialization of function-local statics.
  static Env* default_env = new EmscriptenEnv();
  return default_env;
}

// Set the fs module from JavaScript
void SetFS(emscripten::val fs) {
  // Initialize thread-local fs for main thread
  if (tls_fs == nullptr) {
    tls_fs = new emscripten::val(fs);
  } else {
    *tls_fs = fs;
  }

  // Mark fs as initialized
  g_fs_initialized.store(true, std::memory_order_release);

  // If the default environment already exists, it will pick up the new fs through GetFS()
  // No need to recreate the environment
}

// Set the per-file write buffer size from JavaScript.
//
// This has to be a process-wide setting rather than an Options field because
// NewWritableFile() is an Env method and never sees the options that opened the
// DB. It is read once per EmWritableFile, so it must be set before any DB is
// opened - LevelDB.initialize() is the only caller.
void SetFileWriteBuffer(size_t bytes) {
  g_file_write_buffer.store(bytes, std::memory_order_relaxed);
}

}  // namespace leveldb

#endif  // LEVELDB_PLATFORM_EMSCRIPTEN
