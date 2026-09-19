// Copyright (c) 2024 EmLDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
//
// Emscripten bindings for LevelDB using Embind.
// Provides high-performance JavaScript interface to LevelDB C++ API.

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <leveldb/env.h>
#include <leveldb/comparator.h>
#include <leveldb/compressor.h>
#include <leveldb/zlib_compressor.h>
#include <string>
#include <memory>
#include <vector>

using namespace emscripten;

// C++11 compatible make_unique implementation
namespace std {
  template<typename T, typename... Args>
  unique_ptr<T> make_unique(Args &&...args) {
    return unique_ptr<T>(new T(std::forward<Args>(args)...));
  }
}  // namespace std

namespace leveldb {

// ----------------------------------------------------------------------------
// Utility Functions
// ----------------------------------------------------------------------------

// Convert JavaScript Uint8Array to std::string for binary data transfer
static std::string jsArrayToString(const val &js_array) {
  unsigned int length = js_array["length"].as<unsigned int>();
  std::string result;
  result.reserve(length);

  for (unsigned int i = 0; i < length; ++i) {
    result.push_back(static_cast<char>(js_array[i].as<unsigned char>()));
  }
  return result;
}

// Convert std::string to JavaScript Uint8Array
static val stringToJsArray(const std::string &str) {
  val uint8_array = val::global("Uint8Array").new_(str.size());
  for (size_t i = 0; i < str.size(); ++i) {
    uint8_array.set(i, static_cast<unsigned char>(str[i]));
  }
  return uint8_array;
}

// ----------------------------------------------------------------------------
// JavaScript Comparator Wrapper
// ----------------------------------------------------------------------------

// Bridges JavaScript comparison function to LevelDB Comparator interface
class JSComparator: public Comparator {
public:
  JSComparator(val js_compare, const std::string &name)
      : m_jsCompare(js_compare), m_name(name) {}

  virtual ~JSComparator() {}

  virtual int Compare(const Slice &a, const Slice &b) const {
    val a_array = stringToJsArray(std::string(a.data(), a.size()));
    val b_array = stringToJsArray(std::string(b.data(), b.size()));
    return m_jsCompare(a_array, b_array).as<int>();
  }

  virtual const char *Name() const {
    return m_name.c_str();
  }

  // These are optional optimization methods
  virtual void FindShortestSeparator(std::string *, const Slice &) const {}
  virtual void FindShortSuccessor(std::string *) const {}

private:
  val m_jsCompare;
  std::string m_name;

  // No copying allowed
  JSComparator(const JSComparator &);
  void operator=(const JSComparator &);
};

// ----------------------------------------------------------------------------
// Options Wrappers
// ----------------------------------------------------------------------------

class OptionsWrapper {
public:
  OptionsWrapper() {
    m_options.create_if_missing = true;
    // Initialize compressor array to NULL
    for (int i = 0; i < 256; ++i) {
      m_options.compressors[i] = NULL;
    }
  }

  ~OptionsWrapper() {}

  void setCreateIfMissing(bool value) {
    m_options.create_if_missing = value;
  }

  void setErrorIfExists(bool value) {
    m_options.error_if_exists = value;
  }

  void setParanoidChecks(bool value) {
    m_options.paranoid_checks = value;
  }

  void setWriteBufferSize(size_t size) {
    m_options.write_buffer_size = size;
  }

  void setMaxOpenFiles(int num) {
    m_options.max_open_files = num;
  }

  void setBlockSize(size_t size) {
    m_options.block_size = size;
  }

  void setBlockRestartInterval(int interval) {
    m_options.block_restart_interval = interval;
  }

  void setZlibCompression(int level) {
    m_zlibCompressor = std::make_unique<ZlibCompressor>(level);
    m_options.compressors[(unsigned char)m_zlibCompressor->uniqueCompressionID] =
      m_zlibCompressor.get();
  }

  void setZlibRawCompression(int level) {
    m_zlibRawCompressor = std::make_unique<ZlibCompressorRaw>(level);
    m_options.compressors[(unsigned char)m_zlibRawCompressor->uniqueCompressionID] =
      m_zlibRawCompressor.get();
  }

  void setComparator(val js_compare, const std::string &name) {
    m_comparator = std::make_unique<JSComparator>(js_compare, name);
    m_options.comparator = m_comparator.get();
  }

  void setCache(size_t capacity) {
    m_cache = std::unique_ptr<Cache>(NewLRUCache(capacity));
    m_options.block_cache = m_cache.get();
  }

  void setBloomFilter(int bits_per_key) {
    m_filterPolicy = std::unique_ptr<const FilterPolicy>(
        NewBloomFilterPolicy(bits_per_key)
    );
    m_options.filter_policy = m_filterPolicy.get();
  }

  Options &get() { return m_options; }

private:
  Options m_options;
  std::unique_ptr<JSComparator> m_comparator;
  std::unique_ptr<Cache> m_cache;
  std::unique_ptr<const FilterPolicy> m_filterPolicy;
  std::unique_ptr<ZlibCompressor> m_zlibCompressor;
  std::unique_ptr<ZlibCompressorRaw> m_zlibRawCompressor;

  // No copying allowed
  OptionsWrapper(const OptionsWrapper &);
  void operator=(const OptionsWrapper &);
};

class ReadOptionsWrapper {
public:
  ReadOptionsWrapper() {}
  ~ReadOptionsWrapper() {}

  void setVerifyChecksums(bool value) {
    m_options.verify_checksums = value;
  }

  void setFillCache(bool value) {
    m_options.fill_cache = value;
  }

  ReadOptions &get() { return m_options; }

private:
  ReadOptions m_options;

  // No copying allowed
  ReadOptionsWrapper(const ReadOptionsWrapper &);
  void operator=(const ReadOptionsWrapper &);
};

class WriteOptionsWrapper {
public:
  WriteOptionsWrapper() {}
  ~WriteOptionsWrapper() {}

  void setSync(bool value) {
    m_options.sync = value;
  }

  WriteOptions &get() { return m_options; }

private:
  WriteOptions m_options;

  // No copying allowed
  WriteOptionsWrapper(const WriteOptionsWrapper &);
  void operator=(const WriteOptionsWrapper &);
};

// ----------------------------------------------------------------------------
// Iterator Wrapper
// ----------------------------------------------------------------------------

class IteratorWrapper {
public:
  explicit IteratorWrapper(Iterator *iter): m_iter(iter) {}

  ~IteratorWrapper() {}

  bool valid() const {
    return m_iter->Valid();
  }

  void seekToFirst() {
    m_iter->SeekToFirst();
  }

  void seekToLast() {
    m_iter->SeekToLast();
  }

  void seek(const val &key) {
    std::string key_str = jsArrayToString(key);
    m_iter->Seek(key_str);
  }

  void next() {
    m_iter->Next();
  }

  void prev() {
    m_iter->Prev();
  }

  val key() const {
    if (!m_iter->Valid()) {
      return val::undefined();
    }
    Slice k = m_iter->key();
    return stringToJsArray(std::string(k.data(), k.size()));
  }

  val value() const {
    if (!m_iter->Valid()) {
      return val::undefined();
    }
    Slice v = m_iter->value();
    return stringToJsArray(std::string(v.data(), v.size()));
  }

  std::string getError() const {
    Status status = m_iter->status();
    if (!status.ok()) {
      return status.ToString();
    }
    return "";
  }

private:
  std::unique_ptr<Iterator> m_iter;

  // No copying allowed
  IteratorWrapper(const IteratorWrapper &);
  void operator=(const IteratorWrapper &);
};

// ----------------------------------------------------------------------------
// WriteBatch Wrapper
// ----------------------------------------------------------------------------

class WriteBatchWrapper {
public:
  WriteBatchWrapper() {}
  ~WriteBatchWrapper() {}

  void put(const val &key, const val &value) {
    std::string key_str = jsArrayToString(key);
    std::string value_str = jsArrayToString(value);
    m_batch.Put(key_str, value_str);
  }

  void del(const val &key) {
    std::string key_str = jsArrayToString(key);
    m_batch.Delete(key_str);
  }

  void clear() {
    m_batch.Clear();
  }

  WriteBatch &get() { return m_batch; }

private:
  WriteBatch m_batch;

  // No copying allowed
  WriteBatchWrapper(const WriteBatchWrapper &);
  void operator=(const WriteBatchWrapper &);
};

// ----------------------------------------------------------------------------
// Database Wrapper
// ----------------------------------------------------------------------------

class LevelDBWrapper {
public:
  LevelDBWrapper() {}

  ~LevelDBWrapper() {}

  void open(const std::string &path, OptionsWrapper &options) {
    m_path = path;
    DB *db = NULL;
    Status status = DB::Open(options.get(), path, &db);

    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }

    m_db.reset(db);
  }

  void close() {
    m_db.reset();
  }

  void put(const val &key, const val &value, WriteOptionsWrapper &options) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    std::string key_str = jsArrayToString(key);
    std::string value_str = jsArrayToString(value);

    Status status = m_db->Put(options.get(), key_str, value_str);
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
  }

  val get(const val &key, ReadOptionsWrapper &options) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    std::string key_str = jsArrayToString(key);
    std::string value_str;

    Status status = m_db->Get(options.get(), key_str, &value_str);

    if (status.IsNotFound()) {
      return val::undefined();
    }

    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }

    return stringToJsArray(value_str);
  }

  void del(const val &key, WriteOptionsWrapper &options) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    std::string key_str = jsArrayToString(key);

    Status status = m_db->Delete(options.get(), key_str);
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
  }

  void write(WriteBatchWrapper &batch, WriteOptionsWrapper &options) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    Status status = m_db->Write(options.get(), &batch.get());
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
  }

  IteratorWrapper *createIterator(ReadOptionsWrapper &options) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    Iterator *iter = m_db->NewIterator(options.get());
    return new IteratorWrapper(iter);
  }

  void compactRange(const val &start_key, const val &limit_key) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    std::string start_str, limit_str;
    Slice start_slice, limit_slice;
    Slice *start = NULL;
    Slice *limit = NULL;

    if (!start_key.isUndefined() && !start_key.isNull()) {
      start_str = jsArrayToString(start_key);
      start_slice = Slice(start_str);
      start = &start_slice;
    }

    if (!limit_key.isUndefined() && !limit_key.isNull()) {
      limit_str = jsArrayToString(limit_key);
      limit_slice = Slice(limit_str);
      limit = &limit_slice;
    }

    m_db->CompactRange(start, limit);
  }

  std::string getProperty(const std::string &property) {
    if (!m_db) {
      throw std::runtime_error("Database is not open");
    }

    std::string value;
    if (m_db->GetProperty(property, &value)) {
      return value;
    }
    return "";
  }

  static void destroy(const std::string &path, OptionsWrapper &options) {
    Status status = DestroyDB(path, options.get());
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
  }

  static void repair(const std::string &path, OptionsWrapper &options) {
    Status status = RepairDB(path, options.get());
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
  }

private:
  std::unique_ptr<DB> m_db;
  std::string m_path;

  // No copying allowed
  LevelDBWrapper(const LevelDBWrapper &);
  void operator=(const LevelDBWrapper &);
};

}  // namespace leveldb

// ----------------------------------------------------------------------------
// Emscripten Bindings
// ----------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(leveldb) {
  using namespace leveldb;

  // Options classes
  class_<OptionsWrapper>("Options")
      .constructor<>()
      .function("setCreateIfMissing", &OptionsWrapper::setCreateIfMissing)
      .function("setErrorIfExists", &OptionsWrapper::setErrorIfExists)
      .function("setParanoidChecks", &OptionsWrapper::setParanoidChecks)
      .function("setWriteBufferSize", &OptionsWrapper::setWriteBufferSize)
      .function("setMaxOpenFiles", &OptionsWrapper::setMaxOpenFiles)
      .function("setBlockSize", &OptionsWrapper::setBlockSize)
      .function("setBlockRestartInterval", &OptionsWrapper::setBlockRestartInterval)
      .function("setZlibCompression", &OptionsWrapper::setZlibCompression)
      .function("setZlibRawCompression", &OptionsWrapper::setZlibRawCompression)
      .function("setComparator", &OptionsWrapper::setComparator)
      .function("setCache", &OptionsWrapper::setCache)
      .function("setBloomFilter", &OptionsWrapper::setBloomFilter)
      ;

  class_<ReadOptionsWrapper>("ReadOptions")
      .constructor<>()
      .function("setVerifyChecksums", &ReadOptionsWrapper::setVerifyChecksums)
      .function("setFillCache", &ReadOptionsWrapper::setFillCache)
      ;

  class_<WriteOptionsWrapper>("WriteOptions")
      .constructor<>()
      .function("setSync", &WriteOptionsWrapper::setSync)
      ;

  // Iterator
  class_<IteratorWrapper>("Iterator")
      .function("valid", &IteratorWrapper::valid)
      .function("seekToFirst", &IteratorWrapper::seekToFirst)
      .function("seekToLast", &IteratorWrapper::seekToLast)
      .function("seek", &IteratorWrapper::seek)
      .function("next", &IteratorWrapper::next)
      .function("prev", &IteratorWrapper::prev)
      .function("key", &IteratorWrapper::key)
      .function("value", &IteratorWrapper::value)
      .function("getError", &IteratorWrapper::getError)
      ;

  // WriteBatch
  class_<WriteBatchWrapper>("WriteBatch")
      .constructor<>()
      .function("put", &WriteBatchWrapper::put)
      .function("del", &WriteBatchWrapper::del)
      .function("clear", &WriteBatchWrapper::clear)
      ;

  // Database
  class_<LevelDBWrapper>("DB")
      .constructor<>()
      .function("open", &LevelDBWrapper::open)
      .function("close", &LevelDBWrapper::close)
      .function("put", &LevelDBWrapper::put)
      .function("get", &LevelDBWrapper::get)
      .function("del", &LevelDBWrapper::del)
      .function("write", &LevelDBWrapper::write)
      .function("createIterator", &LevelDBWrapper::createIterator, allow_raw_pointers())
      .function("compactRange", &LevelDBWrapper::compactRange)
      .function("getProperty", &LevelDBWrapper::getProperty)
      .class_function("destroy", &LevelDBWrapper::destroy)
      .class_function("repair", &LevelDBWrapper::repair)
      ;
}
