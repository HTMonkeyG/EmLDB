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
thread_local emscripten::val *tls_fs = nullptr;

// Global flag to track if fs has been initialized
static std::atomic<bool> g_fs_initialized(false);

// Helper structure for StartThread
struct StartThreadState {
  void (*user_function)(void *);
  void *arg;
};

EM_JS(emscripten::EM_VAL, requireFS, (), {
  return Emval.toHandle(require("fs"));
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

Status FsError(const std::string &path, const std::string &operation,
  const emscripten::val &error) {
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
  } catch (...) {
  }
  if (code == "ENOENT") return Status::NotFound(path, message);
  return Status::IOError(path, message);
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
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) {
      }
    }
  }

  virtual Status Read(size_t n, Slice *result, char *scratch) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    try {
      emscripten::val fs = GetFS();
      emscripten::val target = emscripten::val(
        emscripten::typed_memory_view(n, reinterpret_cast<unsigned char *>(scratch)));
      size_t bytesRead = fs.call<size_t>(
        "readSync", m_fd, target, 0, n, emscripten::val::null());
      *result = Slice(scratch, bytesRead);
      return Status::OK();
    } catch (const emscripten::val &error) {
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
    } catch (const emscripten::val &error) {
      return FsError(m_path, "skip", error);
    }
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
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) {
      }
    }
  }

  bool IsValid() const {
    return m_fd != -1;
  }

  virtual Status Read(u64 offset, size_t n, Slice *result, char *scratch) const override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    try {
      emscripten::val fs = GetFS();
      emscripten::val target = emscripten::val(
        emscripten::typed_memory_view(n, reinterpret_cast<unsigned char *>(scratch)));
      size_t bytesRead = fs.call<size_t>(
        "readSync", m_fd, target, 0, n, static_cast<double>(offset));
      *result = Slice(scratch, bytesRead);
      return Status::OK();
    } catch (const emscripten::val &error) {
      *result = Slice();
      return FsError(m_path, "random read", error);
    }
  }

private:
  std::string m_path;
  i32 m_fd;
};

// Writable file implementation using Node.js fs.open/write/close
class EmWritableFile: public WritableFile {
public:
  EmWritableFile(const std::string &path, i32 fd)
    : m_path(path), m_fd(fd) { }

  EmWritableFile(const EmWritableFile &) = delete;
  EmWritableFile &operator=(const EmWritableFile &) = delete;

  virtual ~EmWritableFile() noexcept {
    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) {
      }
    }
  }

  bool IsValid() const {
    return m_fd != -1;
  }

  virtual Status Append(const Slice &data) override {
    if (m_fd == -1) {
      return Status::IOError(m_path, "file not open");
    }

    try {
      emscripten::val fs = GetFS();
      size_t offset = 0;
      while (offset < data.size()) {
        const size_t remaining = data.size() - offset;
        emscripten::val buffer = emscripten::val(
          emscripten::typed_memory_view(remaining,
            reinterpret_cast<const unsigned char *>(data.data() + offset)));
        size_t bytesWritten = fs.call<size_t>("writeSync", m_fd, buffer);
        if (bytesWritten == 0 || bytesWritten > remaining) {
          return Status::IOError(m_path, "write made no progress");
        }
        offset += bytesWritten;
      }
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(m_path, "write", error);
    }
  }

  virtual Status Close() override {
    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
        m_fd = -1;
      } catch (const emscripten::val &error) {
        return FsError(m_path, "close", error);
      }
    }
    return Status::OK();
  }

  virtual Status Flush() override {
    // Node.js writeSync is synchronous, data is already flushed
    return Status::OK();
  }

  virtual Status Sync() override {
    if (m_fd == -1) {
      return Status::IOError(m_path, "file not open");
    }

    try {
      emscripten::val fs = GetFS();
      fs.call<void>("fsyncSync", m_fd);
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(m_path, "fsync", error);
    }
  }

private:
  std::string m_path;
  i32 m_fd;
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
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", m_fd);
      } catch (...) { }
    }
  }

  virtual void Logv(const char *format, va_list ap) override {
    if (m_fd == -1) return;

    char buffer[500];
    int len = vsnprintf(buffer, sizeof(buffer), format, ap);
    if (len > 0 && (len < sizeof(buffer) - 1) && buffer[len - 1] != '\n') {
      buffer[len - 1] = '\n';
      buffer[len] = '0';
    }

    if (m_fd != -1) {
      try {
        emscripten::val fs = GetFS();
        emscripten::val buf = emscripten::val(emscripten::typed_memory_view(
          static_cast<size_t>(len),
          reinterpret_cast<const unsigned char *>(buffer)
        ));
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
  EmscriptenEnv()
    : m_bgsignal(&m_mutex), m_bgStarted(false), m_shutdown(false) { }

  virtual ~EmscriptenEnv() {
    // Shut down background thread
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
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("r"));
      *result = new EmSequentialFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(fname, "open sequential file", error);
    }
  }

  virtual Status NewRandomAccessFile(const std::string &fname, RandomAccessFile **result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("r"));
      *result = new EmRandomAccessFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(fname, "open random access file", error);
    }
  }

  virtual Status NewWritableFile(const std::string &fname, WritableFile **result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("w"));
      *result = new EmWritableFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(fname, "create writable file", error);
    }
  }

  virtual Status NewAppendableFile(const std::string &fname, WritableFile **result) override {
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("a"));
      *result = new EmWritableFile(fname, fdNum);
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(fname, "open appendable file", error);
    }
  }

  virtual bool FileExists(const std::string &fname) override {
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

  virtual Status GetChildren(const std::string &dir, std::vector<std::string> *result) override {
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
    } catch (const emscripten::val &error) {
      return FsError(dir, "read directory", error);
    }
  }

  virtual Status DeleteFile(const std::string &fname) override {
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
    } catch (const emscripten::val &error) {
      return FsError(fname, "delete file", error);
    }
  }

  virtual Status CreateDir(const std::string &dirname) override {
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
    } catch (const emscripten::val &error) {
      return FsError(dirname, "create directory", error);
    }
  }

  virtual Status DeleteDir(const std::string &dirname) override {
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
    } catch (const emscripten::val &error) {
      return FsError(dirname, "delete directory", error);
    }
  }

  virtual Status GetFileSize(const std::string &fname, uint64_t *file_size) override {
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
    } catch (const emscripten::val &error) {
      if (fdNum != -1) {
        try {
          GetFS().call<void>("closeSync", fdNum);
        } catch (...) {
        }
      }
      return FsError(fname, "get file size", error);
    }
  }

  virtual Status RenameFile(const std::string &src, const std::string &target) override {
    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      if (!fs.call<bool>("existsSync", src))
        return Status::NotFound(src);
      fs.call<void>("renameSync", src, target);
      return Status::OK();
    } catch (const emscripten::val &error) {
      return FsError(src, "rename to " + target, error);
    }
  }

  virtual Status LockFile(const std::string &fname, FileLock **lock) override {
    *lock = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Check if already locked by this process
    if (!m_locks.Insert(fname)) {
      return Status::IOError("lock " + fname, "already held by process");
    }

    // Node core does not expose flock. Use an atomically-created sidecar as a
    // fail-safe cross-process guard. It is removed on clean unlock; after a
    // crash it intentionally requires explicit operator recovery rather than
    // guessing that another process's lock is stale.
    const std::string guardName = fname;
    i32 fdNum = -1;
    try {
      emscripten::val fs = GetFS();
      fdNum = fs.call<i32>("openSync", guardName, std::string("wx"));
      std::string owner = "pid=" + std::to_string(
        emscripten::val::global("process")["pid"].as<i32>()) + "\n";
      emscripten::val ownerBytes = emscripten::val::global("Buffer").call<emscripten::val>(
        "from", owner);
      fs.call<size_t>("writeSync", fdNum, ownerBytes);

      EmFileLock *my_lock = new EmFileLock;
      my_lock->fd_ = fdNum;
      my_lock->name_ = fname;
      my_lock->guard_name_ = guardName;
      *lock = my_lock;
      return Status::OK();
    } catch (const emscripten::val &error) {
      if (fdNum != -1) {
        try {
          emscripten::val fs = GetFS();
          fs.call<void>("closeSync", fdNum);
          if (fs.call<bool>("existsSync", guardName)) fs.call<void>("unlinkSync", guardName);
        } catch (...) { }
      }
      m_locks.Remove(fname);
      return FsError(guardName, "acquire database lock", error);
    }
  }

  virtual Status UnlockFile(FileLock *lock) override {
    EmFileLock *my_lock = reinterpret_cast<EmFileLock *>(lock);
    Status status = Status::OK();
    if (my_lock->fd_ != -1 && HasFS()) {
      try {
        emscripten::val fs = GetFS();
        fs.call<void>("closeSync", my_lock->fd_);
        if (fs.call<bool>("existsSync", my_lock->guard_name_))
          fs.call<void>("unlinkSync", my_lock->guard_name_);
      } catch (const emscripten::val &error) {
        status = FsError(my_lock->guard_name_, "release database lock", error);
      }
    }
    m_locks.Remove(my_lock->name_);
    delete my_lock;
    return status;
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
    *result = nullptr;

    if (!HasFS()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    try {
      emscripten::val fs = GetFS();
      i32 fdNum = fs.call<i32>("openSync", fname, std::string("w"));
      *result = new EmLogger(fdNum);
      return Status::OK();
    } catch (const emscripten::val &error) {
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

  // Entry per Schedule() call
  struct BGItem {
    void *arg;
    void (*function)(void *);
  };
  typedef std::deque<BGItem> BGQueue;
  BGQueue m_queue;
};

}  // namespace leveldb

   // Export the default environment
namespace leveldb {

Env *Env::Default() {
  // C++11 guarantees thread-safe initialization of function-local statics.
  static Env *default_env = new EmscriptenEnv();
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

}  // namespace leveldb

#endif  // LEVELDB_PLATFORM_EMSCRIPTEN
