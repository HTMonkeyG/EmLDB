#include "emldb_utils.h"
#include <emscripten/bind.h>

// Convert JavaScript Uint8Array to std::string for binary data transfer.
std::string jsArrayToString(const emscripten::val &arr) {
  const size_t length = arr["byteLength"].as<size_t>();
  std::string str(length, '\0');
  if (length != 0) {
    emscripten::val destination = emscripten::val(
      emscripten::typed_memory_view(length,
        reinterpret_cast<uint8_t *>(&str[0])));
    destination.call<void>("set", arr);
  }
  return str;
}

// Convert std::string to JavaScript Uint8Array.
emscripten::val stringToJsArray(const std::string &str) {
  emscripten::val arr = emscripten::val::global("Uint8Array").new_(str.size());
  if (!str.empty()) {
    emscripten::val source = emscripten::val(
      emscripten::typed_memory_view(str.size(),
        reinterpret_cast<const uint8_t *>(str.data())));
    arr.call<void>("set", source);
  }
  return arr;
}
