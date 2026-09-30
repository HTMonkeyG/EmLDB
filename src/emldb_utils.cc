#include "emldb_utils.h"

// Convert JavaScript Uint8Array to std::string for binary data transfer.
std::string jsArrayToString(const emscripten::val &arr) {
  i32 length = arr["length"].as<u32>();
  std::string str;
  str.reserve(length);

  for (i32 i = 0; i < length; ++i) {
    str.push_back(static_cast<i08>(arr[i].as<u08>()));
  }
  return str;
}

// Convert std::string to JavaScript Uint8Array.
emscripten::val stringToJsArray(const std::string &str) {
  emscripten::val arr = emscripten::val::global("Uint8Array").new_(str.size());
  for (size_t i = 0; i < str.size(); ++i) {
    arr.set(i, static_cast<u08>(str[i]));
  }
  return arr;
}
