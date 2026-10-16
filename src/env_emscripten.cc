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

// ----------------------------------------------------------------------------
// Node fs bridge
// ----------------------------------------------------------------------------
//
// Every filesystem call is made from an EM_JS body that catches the JavaScript
// exception itself and reports the failure through an out-of-band return code.
//
// Going through emscripten::val's call() instead - as this file used to - cannot
// work. A JavaScript exception raised inside such a call is not a C++ exception,
// so the `catch (const emscripten::val&)` guards never ran: the error unwound
// through the wasm frames as a raw JavaScript throw, skipping every destructor
// and the stack pointer restore on the way out. That leaked memory and stack on
// each filesystem error. Here the JS side never throws, so C++ stays in control
// and can turn the failure into a Status.
//
// Return codes:
//   kEmFsOk       - the operation succeeded; out parameters are valid
//   kEmFsNotFound - ENOENT; callers map this to Status::NotFound
//   kEmFsError    - anything else; `message` holds the reason
//
// `message` is always NUL-terminated and truncated to the caller's capacity.

const int kEmFsOk = 0;
const int kEmFsNotFound = 1;
const int kEmFsError = 2;

EM_JS(int, emJsFsOpen, (const char* path, const char* flags, int* fdOut, char* message, size_t messageCap), {
  try {
    HEAP32[fdOut >> 2] = require('fs').openSync(UTF8ToString(path), UTF8ToString(flags));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(int, emJsFsClose, (int fd, char* message, size_t messageCap), {
  try {
    require('fs').closeSync(fd);
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

// A negative `position` reads from the descriptor's own offset.
EM_JS(int, emJsFsRead, (int fd, uint8_t* dst, size_t length, double position,
                        size_t* readOut, char* message, size_t messageCap), {
  try {
    var target = HEAPU8.subarray(dst, dst + length);
    HEAPU32[readOut >> 2] = require('fs').readSync(
      fd, target, 0, length, position < 0 ? null : position);
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(int, emJsFsWrite, (int fd, const uint8_t* src, size_t length,
                         size_t* writtenOut, char* message, size_t messageCap), {
  try {
    HEAPU32[writtenOut >> 2] = require('fs').writeSync(fd, HEAPU8.subarray(src, src + length));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(int, emJsFsFsync, (int fd, char* message, size_t messageCap), {
  try {
    require('fs').fsyncSync(fd);
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(int, emJsFsExists, (const char* path), {
  try {
    return require('fs').existsSync(UTF8ToString(path)) ? 1 : 0;
  } catch (error) {
    return 0;
  }
});

EM_JS(int, emJsFsIsDirectory, (const char* path, int* isDirectoryOut, char* message, size_t messageCap), {
  try {
    HEAP32[isDirectoryOut >> 2] = require('fs').statSync(UTF8ToString(path)).isDirectory() ? 1 : 0;
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(int, emJsFsSize, (const char* path, double* sizeOut, char* message, size_t messageCap), {
  try {
    HEAPF64[sizeOut >> 3] = require('fs').statSync(UTF8ToString(path)).size;
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(int, emJsFsUnlink, (const char* path, char* message, size_t messageCap), {
  try {
    require('fs').unlinkSync(UTF8ToString(path));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(int, emJsFsMkdir, (const char* path, char* message, size_t messageCap), {
  try {
    require('fs').mkdirSync(UTF8ToString(path));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(int, emJsFsRmdir, (const char* path, char* message, size_t messageCap), {
  try {
    require('fs').rmdirSync(UTF8ToString(path));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(int, emJsFsRename, (const char* from, const char* to, char* message, size_t messageCap), {
  try {
    require('fs').renameSync(UTF8ToString(from), UTF8ToString(to));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

// Writes the directory listing as NUL-separated names and reports how many
// bytes it needs, so the caller can retry with a bigger buffer.
// Returns kEmFsOk, 1 when `capacity` is too small, or kEmFsError.
EM_JS(int, emJsFsReaddir, (const char* path, uint8_t* buffer, size_t capacity,
                           size_t* usedOut, size_t* neededOut, char* message, size_t messageCap), {
  try {
    var names = require('fs').readdirSync(UTF8ToString(path));
    var needed = 0;
    var i;
    for (i = 0; i < names.length; i++) needed += lengthBytesUTF8(names[i]) + 1;
    HEAPU32[neededOut >> 2] = needed;
    if (needed > capacity) {
      HEAPU32[usedOut >> 2] = 0;
      return 1;
    }
    var at = 0;
    for (i = 0; i < names.length; i++) {
      var length = lengthBytesUTF8(names[i]) + 1;
      stringToUTF8(names[i], buffer + at, length);
      at += length;
    }
    HEAPU32[usedOut >> 2] = needed;
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(int, emJsProcessPid, (), {
  return process.pid;
});

// performance.now() is monotonic; Date.now() is only a fallback.
EM_JS(double, emJsNowMicros, (), {
  return (typeof performance !== 'undefined')
    ? (performance.timeOrigin + performance.now()) * 1000
    : Date.now() * 1000;
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

// Check if fs is available
inline bool HasFS() {
  return g_fs_initialized.load(std::memory_order_acquire);
}

// Turn one of the kEmFs* codes above into a LevelDB status.
Status FsError(const std::string& path, const std::string& operation,
               int code, const char* message) {
  std::string text = operation + " failed";
  const char* detail = message == nullptr ? "" : message;
  if (detail[0] != '\0') {
    text += ": ";
    text += detail;
  }
  if (code == kEmFsNotFound) return Status::NotFound(path, text);
  return Status::IOError(path, text);
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
      char message[1] = {};
      emJsFsClose(m_fd, message, sizeof(message));
    }
  }

  virtual Status Read(size_t n, Slice* result, char* scratch) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    char message[256] = {};
    size_t bytesRead = 0;
    const int code = emJsFsRead(m_fd, reinterpret_cast<uint8_t*>(scratch), n, -1,
                                &bytesRead, message, sizeof(message));
    if (code != kEmFsOk) {
      *result = Slice();
      return FsError(m_path, "read", code, message);
    }
    *result = Slice(scratch, bytesRead);
    return Status::OK();
  }

  virtual Status Skip(u64 n) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    // Node does not expose lseek. Advance in bounded chunks instead of
    // allocating a buffer proportional to an arbitrary skip distance.
    const size_t kChunkSize = 64 * 1024;
    std::vector<char> scratch(kChunkSize);
    char message[256] = {};
    while (n > 0) {
      const size_t chunk = n < kChunkSize ? static_cast<size_t>(n) : kChunkSize;
      size_t bytesRead = 0;
      const int code = emJsFsRead(m_fd, reinterpret_cast<uint8_t*>(scratch.data()), chunk, -1,
                                  &bytesRead, message, sizeof(message));
      if (code != kEmFsOk) {
        return FsError(m_path, "skip", code, message);
      }
      if (bytesRead == 0) break;
      n -= bytesRead;
    }
    return Status::OK();
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
      char message[1] = {};
      emJsFsClose(m_fd, message, sizeof(message));
    }
  }

  bool IsValid() const { return m_fd != -1; }

  virtual Status Read(u64 offset, size_t n, Slice* result, char* scratch) const override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    char message[256] = {};
    size_t bytesRead = 0;
    const int code = emJsFsRead(m_fd, reinterpret_cast<uint8_t*>(scratch), n,
                                static_cast<double>(offset), &bytesRead,
                                message, sizeof(message));
    if (code != kEmFsOk) {
      *result = Slice();
      return FsError(m_path, "random read", code, message);
    }
    *result = Slice(scratch, bytesRead);
    return Status::OK();
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
      // Best effort: a destructor cannot report a failure, but dropping the
      // buffer would silently lose acknowledged writes.
      FlushBuffer();
      char message[1] = {};
      emJsFsClose(m_fd, message, sizeof(message));
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

    char message[256] = {};
    const int code = emJsFsClose(m_fd, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(m_path, "close", code, message);
    }
    m_fd = -1;
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

    char message[256] = {};
    const int code = emJsFsFsync(m_fd, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(m_path, "fsync", code, message);
    }
    return Status::OK();
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
    char message[256] = {};
    while (offset < m_buffer.size()) {
      const size_t remaining = m_buffer.size() - offset;
      size_t bytesWritten = 0;
      const int code = emJsFsWrite(
        m_fd, reinterpret_cast<const uint8_t*>(m_buffer.data() + offset), remaining,
        &bytesWritten, message, sizeof(message));
      if (code != kEmFsOk) {
        m_buffer.erase(0, offset);
        return FsError(m_path, "write", code, message);
      }
      if (bytesWritten == 0 || bytesWritten > remaining) {
        m_buffer.erase(0, offset);
        return Status::IOError(m_path, "write made no progress");
      }
      offset += bytesWritten;
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
      char message[1] = {};
      emJsFsClose(m_fd, message, sizeof(message));
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

    // Logging is best effort: a failure here must not disturb the database, and
    // the JS side reports it through the return code rather than by throwing.
    size_t written = 0;
    char message[1] = {};
    emJsFsWrite(m_fd, reinterpret_cast<const uint8_t*>(buffer.data()),
                static_cast<size_t>(required), &written, message, sizeof(message));
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

    char message[256] = {};
    i32 fdNum = -1;
    const int code = emJsFsOpen(fname.c_str(), "r", &fdNum, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "open sequential file", code, message);
    }
    *result = new EmSequentialFile(fname, fdNum);
    return Status::OK();
  }

  virtual Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    i32 fdNum = -1;
    const int code = emJsFsOpen(fname.c_str(), "r", &fdNum, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "open random access file", code, message);
    }
    *result = new EmRandomAccessFile(fname, fdNum);
    return Status::OK();
  }

  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    i32 fdNum = -1;
    const int code = emJsFsOpen(fname.c_str(), "w", &fdNum, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "create writable file", code, message);
    }
    *result = new EmWritableFile(fname, fdNum);
    return Status::OK();
  }

  virtual Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    i32 fdNum = -1;
    const int code = emJsFsOpen(fname.c_str(), "a", &fdNum, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "open appendable file", code, message);
    }
    *result = new EmWritableFile(fname, fdNum);
    return Status::OK();
  }

  virtual bool FileExists(const std::string& fname) override {
    if (!HasFS()) {
      return false;
    }
    return emJsFsExists(fname.c_str()) != 0;
  }

  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // The listing arrives as NUL-separated names, so an empty directory is an
    // empty buffer rather than an ambiguity. A directory that grows between the
    // sizing pass and the copy is retried with the larger size.
    char message[256] = {};
    std::vector<uint8_t> buffer(4096);
    for (int attempt = 0; attempt < 4; ++attempt) {
      size_t used = 0;
      size_t needed = 0;
      const int code = emJsFsReaddir(dir.c_str(), buffer.data(), buffer.size(),
                                     &used, &needed, message, sizeof(message));
      if (code == kEmFsError) {
        return FsError(dir, "read directory", code, message);
      }
      if (code == 1) {
        buffer.resize(needed == 0 ? buffer.size() * 2 : needed);
        continue;
      }

      size_t start = 0;
      while (start < used) {
        const char* name = reinterpret_cast<const char*>(buffer.data() + start);
        const size_t length = strlen(name);
        result->push_back(std::string(name, length));
        start += length + 1;
      }
      return Status::OK();
    }
    return Status::IOError(dir, "read directory failed: the listing kept changing");
  }

  virtual Status DeleteFile(const std::string& fname) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    i32 isDirectory = 0;
    int code = emJsFsIsDirectory(fname.c_str(), &isDirectory, message, sizeof(message));
    if (code == kEmFsNotFound) {
      return Status::NotFound(fname);
    }
    if (code != kEmFsOk) {
      return FsError(fname, "delete file", code, message);
    }
    if (isDirectory != 0) {
      return Status::IOError(fname, "expected a file, found a directory");
    }

    code = emJsFsUnlink(fname.c_str(), message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "delete file", code, message);
    }
    return Status::OK();
  }

  virtual Status CreateDir(const std::string& dirname) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    i32 isDirectory = 0;
    const int probe = emJsFsIsDirectory(dirname.c_str(), &isDirectory, message, sizeof(message));
    if (probe == kEmFsOk) {
      if (isDirectory == 0) {
        return Status::IOError(dirname, "path exists and is not a directory");
      }
      return Status::OK();
    }
    if (probe != kEmFsNotFound) {
      return FsError(dirname, "create directory", probe, message);
    }

    const int code = emJsFsMkdir(dirname.c_str(), message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(dirname, "create directory", code, message);
    }
    return Status::OK();
  }

  virtual Status DeleteDir(const std::string& dirname) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    i32 isDirectory = 0;
    const int probe = emJsFsIsDirectory(dirname.c_str(), &isDirectory, message, sizeof(message));
    if (probe == kEmFsNotFound) {
      return Status::NotFound(dirname);
    }
    if (probe != kEmFsOk) {
      return FsError(dirname, "delete directory", probe, message);
    }
    if (isDirectory == 0) {
      return Status::IOError(dirname, "expected a directory, found a file");
    }

    const int code = emJsFsRmdir(dirname.c_str(), message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(dirname, "delete directory", code, message);
    }
    return Status::OK();
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) override {
    *file_size = 0;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    double size = 0;
    const int code = emJsFsSize(fname.c_str(), &size, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "get file size", code, message);
    }
    *file_size = static_cast<uint64_t>(size);
    return Status::OK();
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    char message[256] = {};
    if (emJsFsExists(src.c_str()) == 0) {
      return Status::NotFound(src);
    }

    const int code = emJsFsRename(src.c_str(), target.c_str(), message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(src, "rename to " + target, code, message);
    }
    return Status::OK();
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
      const i32 pid = emJsProcessPid();
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
        // Safe to throw: this is a C++ exception, so the frames it unwinds
        // through run their destructors and restore the stack pointer, and the
        // JS boundary wrapper turns it into a readable Error. Callers reached
        // from the background thread swallow it in BGThreadWrapper.
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
      // See Schedule(): a C++ throw unwinds cleanly and is reported to the
      // caller as an Error rather than terminating the runtime.
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

    char message[256] = {};
    i32 fdNum = -1;
    const int code = emJsFsOpen(fname.c_str(), "w", &fdNum, message, sizeof(message));
    if (code != kEmFsOk) {
      return FsError(fname, "create logger", code, message);
    }
    *result = new EmLogger(fdNum);
    return Status::OK();
  }

  virtual uint64_t NowMicros() override {
    return static_cast<uint64_t>(emJsNowMicros());
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

  // Neither thread entry point may let an exception escape.
  //
  // Nothing above a pthread entry point can catch one, so an exception that
  // gets out calls std::terminate and takes the whole process - and, in this
  // binding, the caller's wasm runtime - down with it. The exception itself is
  // a reporting channel for LevelDB's foreground calls; from a worker it is
  // only ever a reason to stop the work that raised it.
  static void* BGThreadWrapper(void* arg) {
    try {
      reinterpret_cast<EmscriptenEnv*>(arg)->BGThread();
    } catch (...) {
      // Drop the work item's failure and keep serving the queue: shutting the
      // background thread down here would leave the database unable to compact.
    }
    return nullptr;
  }

  static void* StartThreadWrapper(void* arg) {
    StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
    try {
      state->user_function(state->arg);
    } catch (...) {
    }
    // Runs even when the user function threw, which is what keeps it from
    // leaking the state allocated by StartThread().
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

// Set the fs module from JavaScript.
//
// The module itself is no longer kept: every filesystem call is now an EM_JS
// body that reaches for require('fs') on its own thread, which is what keeps a
// failing call from throwing across the C++ boundary. This entry point remains
// as the readiness handshake the JS layer performs before the first operation.
void SetFS(emscripten::val /*fs*/) {
  g_fs_initialized.store(true, std::memory_order_release);
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
