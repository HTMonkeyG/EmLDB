// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Use of this source code is governed by MIT license.
//
// Port layer for Emscripten platform

#ifndef STORAGE_LEVELDB_PORT_PORT_EMSCRIPTEN_H_
#define STORAGE_LEVELDB_PORT_PORT_EMSCRIPTEN_H_

#include <pthread.h>
#include <stdint.h>
#include <string>
#include <atomic>

// Emscripten is little-endian
#define PLATFORM_IS_LITTLE_ENDIAN true

// Use standard unlocked I/O functions
#define fread_unlocked fread
#define fwrite_unlocked fwrite
#define fflush_unlocked fflush

// Use fsync instead of fdatasync
#define fdatasync fsync

namespace leveldb {
namespace port {

static const bool kLittleEndian = PLATFORM_IS_LITTLE_ENDIAN;
#undef PLATFORM_IS_LITTLE_ENDIAN

class CondVar;

class Mutex {
 public:
  Mutex();
  ~Mutex();

  void Lock();
  void Unlock();
  void AssertHeld() { }

 private:
  friend class CondVar;
  pthread_mutex_t mu_;

  // No copying
  Mutex(const Mutex&);
  void operator=(const Mutex&);
};

class CondVar {
 public:
  explicit CondVar(Mutex* mu);
  ~CondVar();
  void Wait();
  void Signal();
  void SignalAll();
 private:
  pthread_cond_t cv_;
  Mutex* mu_;
};

typedef pthread_once_t OnceType;
#define LEVELDB_ONCE_INIT PTHREAD_ONCE_INIT
extern void InitOnce(OnceType* once, void (*initializer)());

inline bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg) {
  return false;
}

// CRC32C checksum
uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size);

// Atomic pointer implementation
class AtomicPointer {
 private:
  std::atomic<void*> rep_;
 public:
  AtomicPointer() : rep_(nullptr) { }
  explicit AtomicPointer(void* p) : rep_(p) {}
  inline void* NoBarrier_Load() const { return rep_.load(std::memory_order_relaxed); }
  inline void NoBarrier_Store(void* v) { rep_.store(v, std::memory_order_relaxed); }
  inline void* Acquire_Load() const { return rep_.load(std::memory_order_acquire); }
  inline void Release_Store(void* v) { rep_.store(v, std::memory_order_release); }
};

} // namespace port
} // namespace leveldb

#endif  // STORAGE_LEVELDB_PORT_PORT_EMSCRIPTEN_H_
