#ifndef __SRC_EMLDB_H__
#define __SRC_EMLDB_H__

#include <stdint.h>
#include <memory>
#include <stdexcept>
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

// Convert std::string to JavaScript Uint8Array (copy).
emscripten::val stringToJsArray(const std::string &str);

// Zero-copy Uint8Array aliasing the wasm heap at [data, data+size).
// Only valid until the owner's next mutation; see the EM_JS definition.
emscripten::val emJsView(const char *data, size_t size);

// Error type for everything this binding reports to JavaScript.
//
// It has to be a real C++ type. Emscripten's JS-based exception handling routes
// a C++ throw through the invoke_* wrappers, which run every skipped frame's
// epilogue - destructors and the stack pointer restore included. A JavaScript
// throw raised inside an EM_JS body instead satisfies `e !== e+0` in those
// wrappers, so it is rethrown as-is: no destructors run, and the wasm stack
// pointer is never restored, leaking stack on every error until the runtime
// corrupts. See throwJsError().
class JsError: public std::runtime_error {
public:
  explicit JsError(const std::string &message): std::runtime_error(message) { }
};

// Parks the message that belongs to the exception currently unwinding on this
// thread.
//
// A JavaScripter-facing error cannot travel inside our exception: what reaches
// JavaScript is the exception pointer as a bare number, so `e.message` is
// undefined. The message is carried out of band instead and picked up by the
// JS boundary wrapper (LevelDB.initialize installs it) right after the native
// call throws.
void setPendingJsError(const std::string &message);
std::string takePendingJsError();

// Raise an error to the caller.
//
// Throws, rather than throwing from JavaScript, so that C++ unwinding runs to
// completion before the value crosses into JavaScript.
[[noreturn]] void throwJsError(const std::string &message);

#endif
