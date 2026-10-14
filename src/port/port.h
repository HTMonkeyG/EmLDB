// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Tracked platform-selection shim for clean Emscripten builds.

#ifndef STORAGE_LEVELDB_PORT_PORT_H_
#define STORAGE_LEVELDB_PORT_PORT_H_

#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)
#include "port_emscripten.h"
#else
#error "src/port/port.h is only intended for LEVELDB_PLATFORM_EMSCRIPTEN builds"
#endif

#endif  // STORAGE_LEVELDB_PORT_PORT_H_
