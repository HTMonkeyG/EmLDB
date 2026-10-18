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
#include "db/filename.h"
#include "port/port.h"
#include "util/mutexlock.h"
#include "util/emldb_utils.h"
#include "util/emldb_fs.h"

// In common MCBE saves, block_size is typically 163840. Therefore, the file
// buffer is set to twice that value for better results.
static size_t g_fileWriteBuffer = 320 * 1024;

void SetFileBufferSize(size_t size) {
  g_fileWriteBuffer = size;
}

namespace leveldb {

namespace {

// Turn one of the kEmFs* codes above into a LevelDB status.
Status FsError(
  const std::string &path,
  const std::string &operation,
  int code,
  cstring message
) {
  std::string text = operation + " failed";
  cstring detail = message == nullptr ? "" : message;
  if (detail[0] != '\0') {
    text += ": ";
    text += detail;
  }
  return code == emldb::kFsNotFound
    ? Status::NotFound(path, text)
    : Status::IOError(path, text);
}

}  // anonymous namespace

// Sequential file implementation using Node.js fs.open/read/close
class EmSequentialFile: public SequentialFile {
public:
  EmSequentialFile(const std::string &path, i32 fd)
    : m_path(path), m_fd(fd) { }

  EmSequentialFile(const EmSequentialFile &) = delete;
  EmSequentialFile &operator=(const EmSequentialFile &) = delete;

  virtual ~EmSequentialFile() noexcept {
    if (m_fd != -1) {
      char message[1] = {};
      // Ignore exceptions in noexcept.
      emldb::fs::close(m_fd, message, sizeof(message));
    }
  }

  virtual Status Read(size_t n, Slice *result, char *scratch) override {
    if (m_fd == -1) return Status::NotFound(m_path);

    char message[256] = {};
    size_t bytesRead = 0;
    const int code = emldb::fs::read(
      m_fd, (u08 *)scratch, n, -1, &bytesRead, message, sizeof(message));
    if (code != emldb::kFsOk) {
      *result = Slice();
      return FsError(m_path, "read", code, message);
    }

    *result = Slice(scratch, bytesRead);
    return Status::OK();
  }

  virtual Status Skip(u64 n) override {
    if (m_fd == -1) return Status::NotFound(m_path);

    // Node does not expose lseek. Advance in bounded chunks instead of
    // allocating a buffer proportional to an arbitrary skip distance.
    const size_t kChunkSize = 64 * 1024;
    std::vector<char> scratch(kChunkSize);
    char message[256] = {};
    while (n > 0) {
      const size_t chunk = n < kChunkSize ? static_cast<size_t>(n) : kChunkSize;
      size_t bytesRead = 0;
      const int code = emldb::fs::read(
        m_fd, (u08 *)scratch.data(), chunk, -1, &bytesRead, message, sizeof(message));
      if (code != emldb::kFsOk) {
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
class EmRandomAccessFile: public RandomAccessFile {
public:
  EmRandomAccessFile(const std::string &path, i32 fd)
    : m_path(path), m_fd(fd) { }

  EmRandomAccessFile(const EmRandomAccessFile &) = delete;
  EmRandomAccessFile &operator=(const EmRandomAccessFile &) = delete;

  virtual ~EmRandomAccessFile() noexcept {
    if (m_fd != -1) {
      char message[1] = {};
      emldb::fs::close(m_fd, message, sizeof(message));
    }
  }

  virtual Status Read(u64 offset, size_t n, Slice *result, char *scratch) const override {
    if (m_fd == -1) return Status::NotFound(m_path);

    char message[256] = {};
    size_t bytesRead = 0;
    const int code = emldb::fs::read(
      m_fd, (u08 *)scratch, n, (f64)offset, &bytesRead, message, sizeof(message));
    if (code != emldb::kFsOk) {
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
class EmWritableFile: public WritableFile {
public:
  EmWritableFile(const std::string &path, i32 fd)
    : m_path(path), m_fd(fd) { }

  EmWritableFile(const EmWritableFile &) = delete;
  EmWritableFile& operator=(const EmWritableFile &) = delete;

  virtual ~EmWritableFile() noexcept {
    if (m_fd != -1) {
      char message[1];
      emldb::fs::close(m_fd, message, sizeof(message));
    }
  }

  bool IsValid() const { return m_fd != -1; }

  virtual Status Append(const Slice &data) override {
    char message[256] = {};

    if (m_fd == -1) return Status::IOError(m_path, "file not open");

    size_t offset = 0;
    while (offset < data.size()) {
      const size_t remaining = data.size() - offset;
      size_t written = 0;
      const int code = emldb::fs::write(
        m_fd,
        (const u08 *)(data.data() + offset),
        remaining,
        &written,
        message,
        sizeof(message)
      );
      if (code != emldb::kFsOk) {
        return FsError(m_path, "write", code, message);
      }
      if (written == 0 || written > remaining) {
        return Status::IOError(m_path, "write made no progress");
      }
      offset += written;
    }

    return Status::OK();
  }

  virtual Status Close() override {
    char message[256] = {};

    if (m_fd == -1) return Status::OK();

    i32 code = emldb::fs::close(m_fd, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(m_path, "close", code, message);

    m_fd = -1;
    return Status::OK();
  }

  virtual Status Flush() override {
    // NOTE: Node.js has no user buffer.
    return Status::OK();
  }

  virtual Status Sync() override {
    char message[256] = {};

    if (m_fd == -1) return Status::IOError(m_path, "file not open");

    i32 code = emldb::fs::fsync(m_fd, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(m_path, "fsync", code, message);

    return Status::OK();
  }

protected:
  std::string m_path;
  i32 m_fd;
};

// Buffered writable file implementation.
//
// Written content is preferentially appended to the in-thread buffer. This
// buffer is passed to Node.js at a rate of one I/O call per kFileWriteBuffer
// bytes. For objects with immediate set to true, calling Flush() immediately
// performs an I/O call and clears the buffer; otherwise, Flush() does nothing.
//
// Our code replaces the user-space buffer, but LevelDB attempts to call Flush()
// after each SSTable block write completes. If Flush() were made to actually
// write to disk in this case, the efficiency gain from the buffer would be
// reduced.
//
// WARN: This comes at a cost: if the database is not properly closed for various
//       reasons, the data in the buffer will be lost. If this is unacceptable,
//       the user-space buffer must be disabled during LevelDB.initialize(), in
//       which case it falls back to the immediate-write version (EmWritableFile).
class EmBufferedWritableFile final: public EmWritableFile {
public:
  EmBufferedWritableFile(const std::string &path, i32 fd, bool immediate = false)
    : EmWritableFile(path, fd)
    , m_immediate(immediate)
  { m_buffer.reserve(g_fileWriteBuffer); }

  EmBufferedWritableFile(const EmBufferedWritableFile &) = delete;
  EmBufferedWritableFile &operator=(const EmBufferedWritableFile &) = delete;

  virtual ~EmBufferedWritableFile() noexcept {
    if (m_fd != -1) {
      // WARN: A destructor cannot report a failure, but dropping the buffer
      //       would silently lose acknowledged writes.
      m_FlushBuffer();
      char message[1] = {};
      emldb::fs::close(m_fd, message, sizeof(message));
    }
  }

  virtual Status Append(const Slice &data) override {
    if (m_fd == -1) return Status::IOError(m_path, "file not open");

    m_buffer.append(data.data(), data.size());

    if (m_buffer.size() >= g_fileWriteBuffer)
      return m_FlushBuffer();

    return Status::OK();
  }

  virtual Status Close() override {
    if (m_fd == -1) return Status::OK();

    // A failed flush must not close the fd: the caller has to be able to retry,
    // and a closed handle would make the lost bytes unrecoverable.
    Status status = m_FlushBuffer();
    if (!status.ok()) {
      return status;
    }

    char message[256] = {};
    i32 code = emldb::fs::close(m_fd, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(m_path, "close", code, message);

    m_fd = -1;
    return Status::OK();
  }

  virtual Status Flush() override {
    // For immediate buffered writable file, we need to ensure that the file is
    // written after every Flush() operation, in order to reduce potential data
    // lost in .log files.
    return m_immediate ? m_FlushBuffer() : Status::OK();
  }

  virtual Status Sync() override {
    if (m_fd == -1) return Status::IOError(m_path, "file not open");

    Status s = Flush();
    if (!s.ok()) return s;

    char message[256] = {};
    i32 code = emldb::fs::fsync(m_fd, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(m_path, "fsync", code, message);

    return Status::OK();
  }

private:
  // Writes the buffer out in one fs.writeSync, retrying on a short write. The
  // already-written prefix is dropped on failure so a later retry cannot
  // duplicate bytes into the log.
  Status m_FlushBuffer() {
    char message[256] = {};

    if (m_buffer.empty()) return Status::OK();
    if (m_fd == -1) return Status::IOError(m_path, "file not open");

    size_t offset = 0;
    while (offset < m_buffer.size()) {
      const size_t remaining = m_buffer.size() - offset;
      size_t bytesWritten = 0;
      const int code = emldb::fs::write(
        m_fd, reinterpret_cast<const u08 *>(m_buffer.data() + offset), remaining,
        &bytesWritten, message, sizeof(message));
      if (code != emldb::kFsOk) {
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

  std::string m_buffer = "";
  bool m_immediate = false;
};

// File lock backed by an atomically-created cross-process guard file.
class EmFileLock: public FileLock {
public:
  i32 fd_;
  std::string name_;
  std::string guard_name_;
};

// Lock table to prevent multiple locks on same file
class EmLockTable {
private:
  port::Mutex mu_;
  std::set<std::string> locked_files_;

public:
  bool Insert(const std::string &fname) {
    MutexLock l(&mu_);
    return locked_files_.insert(fname).second;
  }

  void Remove(const std::string &fname) {
    MutexLock l(&mu_);
    locked_files_.erase(fname);
  }
};

// Logger implementation using fd
class EmLogger: public Logger {
public:
  EmLogger(i32 fd): m_fd(fd) { }

  virtual ~EmLogger() {
    if (m_fd != -1) {
      char message[1];
      emldb::fs::close(m_fd, message, sizeof(message));
    }
  }

  virtual void Logv(const char *format, va_list ap) override {
    if (m_fd == -1) return;

    char buffer[500];
    int len = vsnprintf(buffer, sizeof(buffer), format, ap);
    if (len > 0 && (len + 1 < sizeof(buffer)) && buffer[len - 1] != '\n') {
      buffer[len] = '\n';
      buffer[len + 1] = '0';
      len++;
    }

    size_t written = 0;
    char message[1];
    emldb::fs::write(
      m_fd,
      (const u08 *)buffer,
      (size_t)len,
      &written,
      message,
      sizeof(message)
    );
  }

private:
  i32 m_fd;
};

// Main Emscripten environment implementation
class EmscriptenEnv: public Env {
private:
  // Helper structure for StartThread
  struct StartThreadState {
    void (*user_function)(void *);
    void *arg;
  };

  // Entry per Schedule() call.
  struct BGItem {
    void *arg;
    void (*function)(void *);
  };
  using BGQueue = std::deque<BGItem>;

public:
  EmscriptenEnv()
    : m_bgsignal(&m_mutex), m_bgStarted(false), m_shutdown(false) { }

  virtual ~EmscriptenEnv() {
    // Shut down background thread.
    {
      MutexLock lock(&m_mutex);
      m_shutdown = true;
      m_bgsignal.Signal();
    }

    if (m_bgStarted) {
      pthread_join(m_bgthread, nullptr);
    }
  }

  virtual Status NewSequentialFile(const std::string &fname, SequentialFile **result) override {
    char message[256] = {};

    *result = nullptr;

    i32 fdNum = -1;
    i32 code = emldb::fs::open(fname.c_str(), "r", &fdNum, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "open sequential file", code, message);

    *result = new EmSequentialFile(fname, fdNum);
    return Status::OK();
  }

  virtual Status NewRandomAccessFile(const std::string &fname, RandomAccessFile **result) override {
    char message[256] = {};

    *result = nullptr;

    i32 fdNum = -1;
    i32 code = emldb::fs::open(fname.c_str(), "r", &fdNum, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "open random access file", code, message);

    *result = new EmRandomAccessFile(fname, fdNum);
    return Status::OK();
  }

  virtual Status NewWritableFile(const std::string &fname, WritableFile **result) override {
    char message[256] = {};

    *result = nullptr;

    i32 fdNum = -1
      , code = emldb::fs::open(fname.c_str(), "w", &fdNum, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "open writable file", code, message);

    if (g_fileWriteBuffer) {
      u64 number;
      FileType filetype;
      bool isLeveldbFile = ParseFileName(fname, &number, &filetype);
      *result = new EmBufferedWritableFile(fname, fdNum, isLeveldbFile && filetype == kLogFile);
    } else {
      *result = new EmWritableFile(fname, fdNum);
    }

    return Status::OK();
  }

  virtual Status NewAppendableFile(const std::string &fname, WritableFile **result) override {
    char message[256] = {};

    *result = nullptr;

    i32 fdNum = -1
      , code = emldb::fs::open(fname.c_str(), "a", &fdNum, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "open appendable file", code, message);

    if (g_fileWriteBuffer) {
      u64 number;
      FileType filetype;
      bool isLeveldbFile = ParseFileName(fname, &number, &filetype);
      *result = new EmBufferedWritableFile(fname, fdNum, isLeveldbFile && filetype == kLogFile);
    } else {
      *result = new EmWritableFile(fname, fdNum);
    }

    return Status::OK();
  }

  virtual bool FileExists(const std::string &fname) override {
    return emldb::fs::exists(fname.c_str());
  }

  virtual Status GetChildren(const std::string &dir, std::vector<std::string> *result) override {
    char message[256] = {};

    result->clear();

    // The listing arrives as NUL-separated names, so an empty directory is an
    // empty buffer rather than an ambiguity. A directory that grows between the
    // sizing pass and the copy is retried with the larger size.
    std::vector<uint8_t> buffer(4096);
    for (int attempt = 0; attempt < 4; ++attempt) {
      size_t used = 0;
      size_t needed = 0;
      const int code = emldb::fs::readdir(
        dir.c_str(), buffer.data(), buffer.size(),
        &used, &needed, message, sizeof(message));
      if (code == emldb::kFsError)
        return FsError(dir, "read directory", code, message);

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

  virtual Status DeleteFile(const std::string &fname) override {
    char message[256] = {};

    i32 isDirectory = 0;
    i32 code = emldb::fs::isdir(fname.c_str(), &isDirectory, message, sizeof(message));
    if (code == emldb::kFsNotFound)
      return Status::NotFound(fname);
    if (code != emldb::kFsOk)
      return FsError(fname, "delete file", code, message);
    if (isDirectory != 0)
      return Status::IOError(fname, "expected a file, found a directory");

    code = emldb::fs::unlink(fname.c_str(), message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "delete file", code, message);

    return Status::OK();
  }

  virtual Status CreateDir(const std::string &dirname) override {
    char message[256] = {};

    i32 isDirectory = 0;
    i32 code = emldb::fs::isdir(dirname.c_str(), &isDirectory, message, sizeof(message));
    if (code == emldb::kFsOk) {
      if (isDirectory == 0) {
        return Status::IOError(dirname, "path exists and is not a directory");
      }
      return Status::OK();
    }
    if (code == emldb::kFsError)
      return FsError(dirname, "create directory", code, message);

    code = emldb::fs::mkdir(dirname.c_str(), message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(dirname, "create directory", code, message);

    return Status::OK();
  }

  virtual Status DeleteDir(const std::string &dirname) override {
    char message[256] = {};

    i32 isDirectory = 0;
    i32 code = emldb::fs::isdir(dirname.c_str(), &isDirectory, message, sizeof(message));
    if (code == emldb::kFsNotFound)
      return Status::NotFound(dirname);
    if (code != emldb::kFsOk)
      return FsError(dirname, "delete directory", code, message);
    if (isDirectory == 0)
      return Status::IOError(dirname, "expected a directory, found a file");

    code = emldb::fs::rmdir(dirname.c_str(), message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(dirname, "delete directory", code, message);

    return Status::OK();
  }

  virtual Status GetFileSize(const std::string &fname, uint64_t *file_size) override {
    char message[256] = {};

    *file_size = 0;

    f64 size = 0;
    i32 code = emldb::fs::filesize(fname.c_str(), &size, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "get file size", code, message);

    *file_size = static_cast<uint64_t>(size);
    return Status::OK();
  }

  virtual Status RenameFile(const std::string &src, const std::string &target) override {
    char message[256] = {};

    if (!emldb::fs::exists(src.c_str()))
      return Status::NotFound(src);

    i32 code = emldb::fs::rename(src.c_str(), target.c_str(), message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(src, "rename to " + target, code, message);

    return Status::OK();
  }

  virtual Status LockFile(const std::string &fname, FileLock **lock) override {
    *lock = nullptr;

    // Check if already locked by this process.
    if (!m_locks.Insert(fname)) {
      return Status::IOError("lock " + fname, "already held by process");
    }

    // NOTE: Node.js natively does not support any file-exclusive operations,
    //       so this function effectively does nothing.
    EmFileLock *emlock = new EmFileLock;
    emlock->fd_ = -1;
    emlock->name_ = fname;
    *lock = emlock;

    return Status::OK();
  }

  virtual Status UnlockFile(FileLock *lock) override {
    EmFileLock *emlock = reinterpret_cast<EmFileLock *>(lock);
    m_locks.Remove(emlock->name_);
    delete emlock;
    return Status::OK();
  }

  virtual void Schedule(void (*function)(void *), void *arg) override {
    m_mutex.Lock();

    // Start background thread if necessary
    if (!m_bgStarted) {
      m_bgStarted = true;
      if (pthread_create(&m_bgthread, nullptr, &EmscriptenEnv::BGThreadWrapper, this) != 0) {
        m_bgStarted = false;
        m_mutex.Unlock();
        throw std::runtime_error("unable to start LevelDB background thread");
      }
    }

    // Add to task queue
    m_queue.push_back(BGItem());
    m_queue.back().function = function;
    m_queue.back().arg = arg;

    m_bgsignal.Signal();
    m_mutex.Unlock();
  }

  virtual void StartThread(void (*function)(void *arg), void *arg) override {
    StartThreadState *state = new StartThreadState;
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

  virtual Status GetTestDirectory(std::string *result) override {
    *result = "/tmp/leveldbtest";
    return CreateDir(*result);
  }

  virtual Status NewLogger(const std::string &fname, Logger **result) override {
    char message[256] = {};

    *result = nullptr;

    i32 fdNum = -1
      , code = emldb::fs::open(fname.c_str(), "w", &fdNum, message, sizeof(message));
    if (code != emldb::kFsOk)
      return FsError(fname, "create logger", code, message);

    *result = new EmLogger(fdNum);
    return Status::OK();
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
      m_mutex.Lock();

      while (m_queue.empty() && !m_shutdown) {
        m_bgsignal.Wait();
      }

      if (m_shutdown) {
        m_mutex.Unlock();
        break;
      }

      void (*function)(void *) = m_queue.front().function;
      void *arg = m_queue.front().arg;
      m_queue.pop_front();

      m_mutex.Unlock();
      (*function)(arg);
    }
  }

  static void *BGThreadWrapper(void *arg) {
    reinterpret_cast<EmscriptenEnv *>(arg)->BGThread();
    return nullptr;
  }

  static void *StartThreadWrapper(void *arg) {
    StartThreadState *state = reinterpret_cast<StartThreadState *>(arg);
    state->user_function(state->arg);
    delete state;
    return nullptr;
  }

  EmLockTable m_locks;

  // Background thread state
  port::Mutex m_mutex;
  port::CondVar m_bgsignal;
  pthread_t m_bgthread;
  bool m_bgStarted;
  bool m_shutdown;

  BGQueue m_queue;
};

// Export the default environment
Env *Env::Default() {
  // C++11 guarantees thread-safe initialization of function-local statics.
  static Env *s_defaultEnv = new EmscriptenEnv();
  return s_defaultEnv;
}

}  // namespace leveldb

#endif  // LEVELDB_PLATFORM_EMSCRIPTEN
