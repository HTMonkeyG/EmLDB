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
//
// An unusable argument returns SIZE_MAX instead of throwing. A JavaScript throw
// raised here would unwind through the wasm frames above without running their
// destructors or restoring the stack pointer, so the failure is reported back
// to jsArrayToString, which raises a plain C++ error instead.
EM_JS(size_t, emJsCopyIn, (emscripten::EM_VAL src, uint8_t *dst, size_t cap), {
  var view = Emval.toValue(src);
  if (view === null || view === undefined || view.byteLength === undefined) {
    return 4294967295;
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

// subarray() aliases the wasm heap: no allocation, no copy. The result is only
// meaningful until the caller's next operation on the owning object.
EM_JS(emscripten::EM_VAL, emJsViewRaw, (const uint8_t *src, size_t n), {
  return Emval.toHandle(HEAPU8.subarray(src, src + n));
});

// Convert JavaScript Uint8Array to std::string for binary data transfer.
std::string jsArrayToString(const emscripten::val &arr) {
  // Covers every key and the great majority of values, so the common path
  // never allocates a heap string just to size the copy.
  uint8_t scratch[512];
  const size_t length = emJsCopyIn(arr.as_handle(), scratch, sizeof(scratch));
  if (length == static_cast<size_t>(0xffffffffu)) {
    throwJsError("Data must be a string or Uint8Array");
  }
  if (length <= sizeof(scratch)) {
    return std::string(reinterpret_cast<const char *>(scratch), length);
  }

  std::string str(length, '\0');
  emJsCopyIn(arr.as_handle(), reinterpret_cast<uint8_t *>(&str[0]), length);
  return str;
}

// Copy std::string to a JavaScript Uint8Array (owned copy).
emscripten::val stringToJsArray(const std::string &str) {
  if (str.empty()) {
    return emscripten::val::global("Uint8Array").new_(static_cast<size_t>(0));
  }
  return emscripten::val::take_ownership(emJsCopyOut(
    reinterpret_cast<const uint8_t *>(str.data()), str.size()));
}

// Zero-copy view over wasm memory; the caller must not outlive the buffer.
emscripten::val emJsView(const char *data, size_t size) {
  return emscripten::val::take_ownership(
    emJsViewRaw(reinterpret_cast<const uint8_t *>(data), size));
}

// The message belonging to the exception currently unwinding on this thread.
// Per-thread because a failed open or compaction can be reported from a worker.
thread_local std::string tls_pending_js_error;

void setPendingJsError(const std::string &message) {
  tls_pending_js_error = message;
}

std::string takePendingJsError() {
  std::string message;
  message.swap(tls_pending_js_error);
  return message;
}

[[noreturn]] void throwJsError(const std::string &message) {
  // Park the message first: the C++ exception that follows reaches JavaScript as
  // an opaque pointer, and the JS boundary wrapper reads this slot to rebuild a
  // readable Error. Throwing a C++ type - rather than raising a JavaScript
  // error from an EM_JS body - is what makes emscripten's invoke_* wrappers run
  // the destructors of every skipped frame and restore the stack pointer.
  setPendingJsError(message);
  throw JsError(message);
}
