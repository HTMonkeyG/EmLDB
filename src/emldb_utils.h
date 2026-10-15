#ifndef __SRC_EMLDB_H__
#define __SRC_EMLDB_H__

#include <stdint.h>
#include <memory>
#include <string>
#include <utility>
#include <emscripten/val.h>

// ----------------------------------------------------------------------------
// Type definitions
// ----------------------------------------------------------------------------

using i08 = int8_t;
using u08 = uint8_t;
using i16 = int16_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
using cstring = const char *;

// ----------------------------------------------------------------------------
// Utility Functions
// ----------------------------------------------------------------------------

// C++11 compatible make_unique implementation.
namespace emldb {
  template<typename T, typename ...Args>
  std::unique_ptr<T> make_unique(Args &&...args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
  }
}  // namespace emldb

// Convert JavaScript Uint8Array to std::string for binary data transfer.
std::string jsArrayToString(const emscripten::val &arr);

// Convert std::string to JavaScript Uint8Array.
emscripten::val stringToJsArray(const std::string &str);

// Raise a JavaScript Error carrying `message`.
//
// A C++ exception crossing the embind boundary reaches JavaScript as a bare
// pointer, so `catch (e)` in JS sees a number and `e.message` is undefined -
// every diagnostic this library produces would be unreadable. Raising the value
// on the JavaScript side keeps the message intact.
[[noreturn]] void throwJsError(const std::string &message);

#endif
