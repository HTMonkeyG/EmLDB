// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Use of this source code is governed by MIT license.
//
// Emscripten environment implementation using Node.js fs APIs.
// Provides filesystem access for LevelDB running in Emscripten/Node.js.

#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)

#include <pthread.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <emscripten/val.h>
#include <leveldb/env.h>
#include <leveldb/slice.h>
#include "port/port.h"
#include "util/mutexlock.h"
#include "emldb_utils.h"

namespace leveldb {

namespace {

// Global fs module reference (set from JavaScript)
static emscripten::val* g_fs = nullptr;

// Helper structure for StartThread
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};

}  // anonymous namespace

// Sequential file implementation using Node.js fs.open/read/close
class EmSequentialFile : public SequentialFile {
public:
  EmSequentialFile(emscripten::val fs, const std::string& path, i32 fd)
      : m_fs(fs), m_path(path), m_fd(fd) {}

  EmSequentialFile(const EmSequentialFile&) = delete;
  EmSequentialFile& operator=(const EmSequentialFile&) = delete;

  virtual ~EmSequentialFile() noexcept {
    if (m_fd != -1) {
      m_fs["closeSync"](m_fd);
    }
  }

  virtual Status Read(size_t n, Slice* result, char* scratch) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    // Create a Buffer for reading
    emscripten::val buffer = emscripten::val::global("Buffer")["alloc"](n);

    // Read data: fs.readSync(fd, buffer, 0, length, null)
    emscripten::val bytesReadVal = m_fs["readSync"](m_fd, buffer, 0, n, emscripten::val::null());
    i32 bytesRead = bytesReadVal.as<i32>();

    if (bytesRead < 0) {
      *result = Slice();
      return Status::IOError(m_path, "read failed");
    }

    // Copy data from Buffer to scratch
    for (i32 i = 0; i < bytesRead; ++i) {
      scratch[i] = static_cast<char>(buffer[i].as<u08>());
    }

    *result = Slice(scratch, bytesRead);

    return Status::OK();
  }

  virtual Status Skip(u64 n) override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    // Read and discard data to simulate skip
    emscripten::val buffer = emscripten::val::global("Buffer")["alloc"](n);
    emscripten::val bytesReadVal = m_fs["readSync"](m_fd, buffer, 0, n, emscripten::val::null());
    i32 bytesRead = bytesReadVal.as<i32>();

    if (bytesRead < 0) {
      return Status::IOError(m_path, "skip failed");
    }

    return Status::OK();
  }

private:
  emscripten::val m_fs;
  std::string m_path;
  i32 m_fd;
};

// Random access file implementation using fd and pread
class EmRandomAccessFile : public RandomAccessFile {
public:
  EmRandomAccessFile(emscripten::val fs, const std::string& path, i32 fd)
      : m_fs(fs), m_path(path), m_fd(fd) {}

  EmRandomAccessFile(const EmRandomAccessFile&) = delete;
  EmRandomAccessFile& operator=(const EmRandomAccessFile&) = delete;

  virtual ~EmRandomAccessFile() noexcept {
    if (m_fd != -1) {
      m_fs["closeSync"](m_fd);
    }
  }

  bool IsValid() const { return m_fd != -1; }

  virtual Status Read(u64 offset, size_t n, Slice* result, char* scratch) const override {
    if (m_fd == -1) {
      return Status::NotFound(m_path);
    }

    // Create a Buffer for reading
    emscripten::val buffer = emscripten::val::global("Buffer")["alloc"](n);

    // Read at offset: fs.readSync(fd, buffer, 0, length, position)
    emscripten::val bytesReadVal = m_fs["readSync"](m_fd, buffer, 0, n, static_cast<double>(offset));
    i32 bytesRead = bytesReadVal.as<i32>();

    if (bytesRead < 0) {
      *result = Slice();
      return Status::IOError(m_path, "read failed");
    }

    // Copy data from Buffer to scratch
    for (i32 i = 0; i < bytesRead; ++i) {
      scratch[i] = static_cast<char>(buffer[i].as<u08>());
    }

    *result = Slice(scratch, bytesRead);

    return Status::OK();
  }

private:
  emscripten::val m_fs;
  std::string m_path;
  i32 m_fd;
};

// Writable file implementation using Node.js fs.open/write/close
class EmWritableFile : public WritableFile {
public:
  EmWritableFile(emscripten::val fs, const std::string& path, i32 fd)
      : m_fs(fs), m_path(path), m_fd(fd) {}

  EmWritableFile(const EmWritableFile&) = delete;
  EmWritableFile& operator=(const EmWritableFile&) = delete;

  virtual ~EmWritableFile() noexcept {
    if (m_fd != -1) {
      m_fs["closeSync"](m_fd);
    }
  }

  bool IsValid() const { return m_fd != -1; }

  virtual Status Append(const Slice& data) override {
    if (m_fd == -1) {
      return Status::IOError(m_path, "file not open");
    }

    // Convert Slice to Buffer
    emscripten::val buffer = emscripten::val::global("Buffer")["from"](
      emscripten::val(emscripten::typed_memory_view(data.size(),
        reinterpret_cast<const unsigned char*>(data.data())))
    );

    // Write: fs.writeSync(fd, buffer)
    emscripten::val bytesWritten = m_fs["writeSync"](m_fd, buffer);

    if (bytesWritten.as<size_t>() != data.size()) {
      return Status::IOError(m_path, "write incomplete");
    }

    return Status::OK();
  }

  virtual Status Close() override {
    if (m_fd != -1) {
      m_fs["closeSync"](m_fd);
      m_fd = -1;
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

    // fsync: fs.fsyncSync(fd)
    m_fs["fsyncSync"](m_fd);

    return Status::OK();
  }

private:
  emscripten::val m_fs;
  std::string m_path;
  i32 m_fd;
};

// File lock implementation (simplified for single-process environment)
class EmFileLock : public FileLock {
public:
  i32 fd_;
  std::string name_;
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
  EmLogger(emscripten::val fs, i32 fd) : m_fs(fs), m_fd(fd) {}

  virtual ~EmLogger() {
    if (m_fd != -1) {
      m_fs["closeSync"](m_fd);
    }
  }

  virtual void Logv(const char* format, va_list ap) override {
    char buffer[500];
    vsnprintf(buffer, sizeof(buffer), format, ap);

    if (m_fd != -1) {
      size_t len = strlen(buffer);
      emscripten::val buf = emscripten::val::global("Buffer")["from"](
        emscripten::val(emscripten::typed_memory_view(len,
          reinterpret_cast<const unsigned char*>(buffer)))
      );
      m_fs["writeSync"](m_fd, buf);
    }
  }

private:
  emscripten::val m_fs;
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

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Open file: fs.openSync(path, 'r')
    emscripten::val fd = m_fs["openSync"](fname, std::string("r"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      return Status::NotFound(fname);
    }

    *result = new EmSequentialFile(m_fs, fname, fdNum);
    return Status::OK();
  }

  virtual Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    *result = nullptr;

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Open file: fs.openSync(path, 'r')
    emscripten::val fd = m_fs["openSync"](fname, std::string("r"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      return Status::NotFound(fname);
    }

    *result = new EmRandomAccessFile(m_fs, fname, fdNum);
    return Status::OK();
  }

  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Open file for writing: fs.openSync(path, 'w')
    emscripten::val fd = m_fs["openSync"](fname, std::string("w"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      return Status::IOError(fname, "cannot create file");
    }

    *result = new EmWritableFile(m_fs, fname, fdNum);
    return Status::OK();
  }

  virtual Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Open file for appending: fs.openSync(path, 'a')
    emscripten::val fd = m_fs["openSync"](fname, std::string("a"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      return Status::IOError(fname, "cannot open file for append");
    }

    *result = new EmWritableFile(m_fs, fname, fdNum);
    return Status::OK();
  }

  virtual bool FileExists(const std::string& fname) override {
    if (m_fs.isUndefined()) {
      return false;
    }
    // fs.existsSync(path)
    return m_fs["existsSync"](fname).as<bool>();
  }

  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // fs.readdirSync(path)
    emscripten::val files = m_fs["readdirSync"](dir);

    i32 length = files["length"].as<i32>();
    for (i32 i = 0; i < length; ++i) {
      result->push_back(files[i].as<std::string>());
    }

    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) override {
    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // fs.unlinkSync(path)
    m_fs["unlinkSync"](fname);
    return Status::OK();
  }

  virtual Status CreateDir(const std::string& dirname) override {
    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Check if directory already exists
    bool exists = m_fs["existsSync"](dirname).as<bool>();
    if (exists) {
      // Directory already exists, return success
      return Status::OK();
    }

    // fs.mkdirSync(path, { recursive: false })
    emscripten::val options = emscripten::val::object();
    options.set("recursive", false);
    m_fs["mkdirSync"](dirname, options);
    return Status::OK();
  }

  virtual Status DeleteDir(const std::string& dirname) override {
    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // fs.rmdirSync(path)
    m_fs["rmdirSync"](dirname);
    return Status::OK();
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) override {
    *file_size = 0;

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Open, fstat, close using fd
    emscripten::val fd = m_fs["openSync"](fname, std::string("r"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      return Status::NotFound(fname);
    }

    // fs.fstatSync(fd)
    emscripten::val stats = m_fs["fstatSync"](fdNum);
    *file_size = stats["size"].as<u64>();

    m_fs["closeSync"](fdNum);

    return Status::OK();
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) override {
    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Check if source file exists first
    bool exists = m_fs["existsSync"](src).as<bool>();
    if (!exists) {
      // If source doesn't exist, consider it a success (LevelDB behavior)
      return Status::OK();
    }

    // fs.renameSync(oldPath, newPath)
    m_fs["renameSync"](src, target);
    return Status::OK();
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = nullptr;

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Check if already locked by this process
    if (!m_locks.Insert(fname)) {
      return Status::IOError("lock " + fname, "already held by process");
    }

    // In Node.js single-process environment, we need to handle existing lock files
    // Delete stale lock file if it exists
    if (m_fs["existsSync"](fname).as<bool>()) {
      m_fs["unlinkSync"](fname);
    }

    // Create new lock file: fs.openSync(path, 'w')
    emscripten::val fd = m_fs["openSync"](fname, std::string("w"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      m_locks.Remove(fname);
      return Status::IOError("lock " + fname, "cannot create lock file");
    }

    EmFileLock* my_lock = new EmFileLock;
    my_lock->fd_ = fdNum;
    my_lock->name_ = fname;
    *lock = my_lock;

    return Status::OK();
  }

  virtual Status UnlockFile(FileLock* lock) override {
    EmFileLock* my_lock = reinterpret_cast<EmFileLock*>(lock);
    m_locks.Remove(my_lock->name_);

    if (my_lock->fd_ != -1 && !m_fs.isUndefined()) {
      m_fs["closeSync"](my_lock->fd_);
    }

    delete my_lock;
    return Status::OK();
  }

  virtual void Schedule(void (*function)(void*), void* arg) override {
    MutexLock lock(&mu_);

    // Start background thread if necessary
    if (!bgthread_started_) {
      bgthread_started_ = true;
      if (pthread_create(&bgthread_, nullptr, &EmscriptenEnv::BGThreadWrapper, this) != 0) {
        bgthread_started_ = false;
        // Fallback: execute immediately if thread creation fails
        function(arg);
        return;
      }
    }

    // Add to task queue
    queue_.push_back(BGItem());
    queue_.back().function = function;
    queue_.back().arg = arg;

    bgsignal_.Signal();
  }

  virtual void StartThread(void (*function)(void* arg), void* arg) override {
    StartThreadState* state = new StartThreadState;
    state->user_function = function;
    state->arg = arg;

    pthread_t thread;
    if (pthread_create(&thread, nullptr, &StartThreadWrapper, state) != 0) {
      // If thread creation fails, clean up and execute immediately
      delete state;
      function(arg);
      return;
    }

    // Detach the thread so it can run independently
    pthread_detach(thread);
  }

  virtual Status GetTestDirectory(std::string* result) override {
    *result = "/tmp/leveldbtest";
    CreateDir(*result);
    return Status::OK();
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) override {
    *result = nullptr;

    if (m_fs.isUndefined()) {
      return Status::IOError("fs module not initialized", "call SetFS first");
    }

    // Open file for writing: fs.openSync(path, 'w')
    emscripten::val fd = m_fs["openSync"](fname, std::string("w"));
    i32 fdNum = fd.as<i32>();

    if (fdNum < 0) {
      return Status::IOError(fname, "cannot create log file");
    }

    *result = new EmLogger(m_fs, fdNum);
    return Status::OK();
  }

  virtual uint64_t NowMicros() override {
    // Use Date.now() which returns milliseconds since epoch
    emscripten::val date = emscripten::val::global("Date");
    double millis = date["now"]().as<double>();
    return static_cast<u64>(millis * 1000);
  }

  virtual void SleepForMicroseconds(int micros) override {
    // Emscripten doesn't support synchronous sleep in single-threaded mode
    // This is a no-op in browser/Node.js environment
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

  emscripten::val m_fs;
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
  if (g_fs != nullptr) {
    m_fs = *g_fs;
  }
}

static Env* default_env = nullptr;

Env* Env::Default() {
  if (default_env == nullptr) {
    default_env = new EmscriptenEnv();
  }
  return default_env;
}

// Set the fs module from JavaScript
void SetFS(emscripten::val fs) {
  if (g_fs == nullptr) {
    g_fs = new emscripten::val(fs);
  } else {
    *g_fs = fs;
  }

  // If default_env already exists, update its fs reference
  if (default_env != nullptr) {
    delete default_env;
    default_env = new EmscriptenEnv();
  }
}

}  // namespace leveldb

#endif  // LEVELDB_PLATFORM_EMSCRIPTEN
