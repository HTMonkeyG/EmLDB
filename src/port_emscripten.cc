// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Use of this source code is governed by MIT license.
//
// Port layer implementation for Emscripten platform

#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)

#include "port_emscripten.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace leveldb {
namespace port {

// Mutex implementation
Mutex::Mutex() {
  pthread_mutex_init(&mu_, nullptr);
}

Mutex::~Mutex() {
  pthread_mutex_destroy(&mu_);
}

void Mutex::Lock() {
  pthread_mutex_lock(&mu_);
}

void Mutex::Unlock() {
  pthread_mutex_unlock(&mu_);
}

// CondVar implementation
CondVar::CondVar(Mutex* mu) : mu_(mu) {
  pthread_cond_init(&cv_, nullptr);
}

CondVar::~CondVar() {
  pthread_cond_destroy(&cv_);
}

void CondVar::Wait() {
  pthread_cond_wait(&cv_, &mu_->mu_);
}

void CondVar::Signal() {
  pthread_cond_signal(&cv_);
}

void CondVar::SignalAll() {
  pthread_cond_broadcast(&cv_);
}

// InitOnce implementation
void InitOnce(OnceType* once, void (*initializer)()) {
  pthread_once(once, initializer);
}

// CRC32C implementation (software fallback for Emscripten)
// Emscripten doesn't support SSE4.2, so we provide a simple implementation
// that returns 0 to indicate no hardware acceleration is available
uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size) {
  // Return 0 to indicate no hardware acceleration
  // The caller (crc32c.cc) will fall back to the portable implementation
  return 0;
}

}  // namespace port
}  // namespace leveldb

#endif  // LEVELDB_PLATFORM_EMSCRIPTEN
