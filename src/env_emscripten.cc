// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Use of this source code is governed by MIT license.
//
// Emscripten bindings for LevelDB using Embind.
// Provides high-performance JavaScript interface to LevelDB C++ API.

#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)

#include <string.h>
#include <string>
#include <emscripten/val.h>
#include <leveldb/env.h>
#include "emldb_utils.h"

namespace leveldb {

// A file abstraction for reading sequentially through a file
class EmSequentialFile: public SequentialFile {
public:
  EmSequentialFile(
    emscripten::val fs,
    const std::string &path
  )
    : m_fs(fs)
    , m_path(path)
    , m_fd(fs["openSync"](path, "r").as<i32>())
  { }
  EmSequentialFile(const EmSequentialFile &) = delete;
  EmSequentialFile &operator=(const EmSequentialFile &) = delete;

  inline bool IsValid() const { return m_fd != -1; }

  inline virtual ~EmSequentialFile() noexcept { if (IsValid()) m_fs["closeSync"](m_fd); }

  inline virtual Status Read(
    size_t n,
    Slice *result, 
    char *scratch
  ) override {
    if (!IsValid())
      return Status::NotFound(m_path);

    i32 read = m_fs["readSync"](m_fd, "r").as<i32>()

    memcpy(scratch, m_content.c_str() + m_cursor, n);
    m_cursor += n;

    return Status::OK();
  }

  inline virtual Status Skip(
    u64 n
  ) override {
    if (!IsValid())
      return Status::NotFound(m_path);
    if (m_cursor + n > m_content.length())
      return Status::IOError(m_path);
    m_cursor += n;
    return Status::OK();
  }

private:
  emscripten::val m_fs = {};
  std::string m_path = "";
  i32 m_fd = -1;
  size_t m_cursor = 0;
};

// A file abstraction for randomly reading the contents of a file.
class EmRandomAccessFile: public RandomAccessFile {
public:
  EmRandomAccessFile(
    emscripten::val fs,
    const std::string &path
  )
    : m_path(path)
  {
    // Read the entire file into process memory.
    emscripten::val v = fs["readFileSync"](path);
    if (v.isUndefined())
      return;
    m_content = jsArrayToString(v);
    m_valid = true;
  }
  EmRandomAccessFile(const EmRandomAccessFile &) = delete;
  EmRandomAccessFile &operator=(const EmRandomAccessFile &) = delete;

  bool IsValid() const { return m_valid; }

  virtual ~EmRandomAccessFile() noexcept = default;

  virtual Status Read(
    u64 offset,
    size_t n,
    Slice *result,
    char *scratch
  ) const override {
    if (!IsValid())
      return Status::NotFound(m_path);

    if (offset + n > m_content.length())
      return Status::IOError(m_path);

    memcpy(scratch, m_content.c_str() + offset, n);

    return Status::OK();
  }

private:
  std::string m_content = "";
  std::string m_path = "";
  bool m_valid = false;
};

// A file abstraction for sequential writing.  The implementation
// must provide buffering since callers may append small fragments
// at a time to the file.
class EmWritableFile: public WritableFile {
public:
    EmWritableFile(
    emscripten::val fs,
    const std::string &path
  )
    : m_path(path)
  {
    // Read the entire file into process memory.
    emscripten::val v = fs["readFileSync"](path);
    if (v.isUndefined())
      return;
    m_content = jsArrayToString(v);
    m_valid = true;
  }
  EmWritableFile(const EmWritableFile &) = delete;
  EmWritableFile &operator=(const EmWritableFile &) = delete;

  bool IsValid() const { return m_valid; }

  virtual ~EmWritableFile() noexcept = default;

  virtual Status Append(
    const Slice &data
  ) override {

  }

  virtual Status Close() override {

  }

  virtual Status Flush() override {

  }

  virtual Status Sync() override {

  }

private:
};

class EmscriptenEnv: public EnvWrapper {
public:
  EmscriptenEnv() = default;
  ~EmscriptenEnv() = default;

};

}

#endif
