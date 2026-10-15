#include "emldb_utils.h"
#include <emscripten/bind.h>
#include <emscripten/em_js.h>
#include <stdexcept>

// These two conversions sit on the hot path of every put, get and iterator
// step, so they are written as single EM_JS calls. Going through
// emscripten::val needs one JS round trip per operation - read byteLength,
// build a typed_memory_view, call set - and a put/get used seven of them.
// Each crossing is a wasm/JS switch plus a handle refcount, which the profiler
// charged roughly 40% of readrandom to. Below it is two crossings per put/get,
// and the outgoing direction allocates its Uint8Array inside JS rather than
// fetching the constructor through the boundary.

// Copies min(byteLength, cap) bytes into dst and returns the view's real
// byteLength, so the caller can retry with a heap buffer when it did not fit.
// Keeping the length and the copy in one call is what removes the extra
// crossing; oversized keys and values take the two-call retry instead.
EM_JS(size_t, emJsCopyIn, (emscripten::EM_VAL src, uint8_t *dst, size_t cap), {
  var view = Emval.toValue(src);
  if (view === null || view === undefined || view.byteLength === undefined) {
    throw new TypeError("Data must be a string or Uint8Array");
  }
  var length = view.byteLength;
  if (length <= cap) HEAPU8.set(view, dst);
  return length;
});

// slice() already returns a fresh Uint8Array, so the caller gets an owned
// copy without constructing one through the boundary.
EM_JS(emscripten::EM_VAL, emJsCopyOut, (const uint8_t *src, size_t n), {
  return Emval.toHandle(HEAPU8.slice(src, src + n));
});

// Convert JavaScript Uint8Array to std::string for binary data transfer.
std::string jsArrayToString(const emscripten::val &arr) {
  // Covers every key and the great majority of values, so the common path
  // never allocates a heap string just to size the copy.
  uint8_t scratch[512];
  const size_t length = emJsCopyIn(arr.as_handle(), scratch, sizeof(scratch));
  if (length <= sizeof(scratch)) {
    return std::string(reinterpret_cast<const char *>(scratch), length);
  }

  std::string str(length, '\0');
  emJsCopyIn(arr.as_handle(), reinterpret_cast<uint8_t *>(&str[0]), length);
  return str;
}

// Convert std::string to JavaScript Uint8Array.
emscripten::val stringToJsArray(const std::string &str) {
  if (str.empty()) {
    return emscripten::val::global("Uint8Array").new_(static_cast<size_t>(0));
  }
  return emscripten::val::take_ownership(emJsCopyOut(
    reinterpret_cast<const uint8_t *>(str.data()), str.size()));
}

// Throw the error from the JavaScript side so it stays an Error object. See the
// declaration for why the C++ exception below cannot carry the message.
EM_JS(void, emJsThrowError, (const char *message), {
  throw new Error(UTF8ToString(message));
});

[[noreturn]] void throwJsError(const std::string &message) {
  emJsThrowError(message.c_str());
  // Only reached if the JS throw fails to unwind, in which case a C++ throw is
  // the best that is left; it also gives the compiler a non-returning path.
  throw std::runtime_error(message);
}
