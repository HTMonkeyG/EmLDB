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

// CRC32C — Emscripten has no SSE4.2 `crc32` intrinsic that maps to WASM,
// so the upstream code would probe CanAccelerateCRC32C() via a string test
// and always fall back to the per-byte STEP1/STEP4 extend loop. Implementing
// the bulk 4-table path directly as AcceleratedCRC32C avoids the probe and
// matches the 16-byte bulk that db_bench reports as 1639 MB/s on x86.
// WASM has no `crc32` op; this is the same 4-table LE_LOAD32+zext that
// util/crc32c.cc uses as its software extend, compiled to i32.load/shift.
//
// Tables duplicated from util/crc32c.cc to avoid a circular dependency on the
// leveldb static tables (which live in util/crc32c.cc's anonymous namespace).
// 4KB wasm data is negligible next to the 128MB heap. The .inc holds no
// includes of its own so it can be pulled in at namespace scope here.
#include "util/crc32c_tables.inc"

uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
  const uint8_t* e = p + size;
  uint32_t l = crc ^ 0xffffffffu;

  #define STEP1_EM do { int c = (l & 0xff) ^ *p++; l = kTable0[c] ^ (l >> 8); } while(0)
  #define STEP4_EM do {                          \
    uint32_t c = l ^ (uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24)); \
    p += 4;                                      \
    l = kTable3[c & 0xff] ^ kTable2[(c>>8)&0xff] ^ kTable1[(c>>16)&0xff] ^ kTable0[c>>24]; \
  } while(0)

  const uintptr_t pval = reinterpret_cast<uintptr_t>(p);
  const uint8_t* x = reinterpret_cast<const uint8_t*>(((pval + 3) >> 2) << 2);
  if (x <= e) while (p != x) STEP1_EM;
  while ((e - p) >= 16) { STEP4_EM; STEP4_EM; STEP4_EM; STEP4_EM; }
  while ((e - p) >= 4)  STEP4_EM;
  while (p != e)        STEP1_EM;
  #undef STEP4_EM
  #undef STEP1_EM
  return l ^ 0xffffffffu;
}

}  // namespace port
}  // namespace leveldb

#endif  // LEVELDB_PLATFORM_EMSCRIPTEN
