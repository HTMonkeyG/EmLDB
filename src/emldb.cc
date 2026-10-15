// Copyright (c) 2026 HTMonkeyG. All rights reserved.
// Use of this source code is governed by MIT license.
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
#include <cmath>
#include <stdexcept>

#include "emldb_utils.h"

using namespace emscripten;

namespace leveldb {

// Forward declaration of functions from env_emscripten.cc
#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)
void SetFS(emscripten::val fs);
void SetFileWriteBuffer(size_t bytes);
#endif

// ----------------------------------------------------------------------------
// JavaScript Comparator Wrapper
// ----------------------------------------------------------------------------

// Bridges JavaScript comparison function to LevelDB Comparator interface.
class JSComparator: public Comparator {
public:
  JSComparator(val jsFunc, const std::string &name): m_jsCompare(jsFunc), m_name(name) { }
  JSComparator(const JSComparator &) = delete;
  JSComparator &operator=(const JSComparator &) = delete;

  virtual ~JSComparator() noexcept override = default;

  virtual int Compare(const Slice &a, const Slice &b) const override {
    val a_array = stringToJsArray(std::string(a.data(), a.size()));
    val b_array = stringToJsArray(std::string(b.data(), b.size()));
    double result = m_jsCompare(a_array, b_array).as<double>();
    if (!std::isfinite(result)) {
      // Stays a C++ exception: comparisons run on LevelDB's compaction worker,
      // where there is no JavaScript to raise an Error on. Unreachable anyway,
      // since the JS layer rejects comparators on the threaded build.
      throw std::runtime_error("Comparator must return a finite number");
    }
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
  }

  virtual cstring Name() const override { return m_name.c_str(); }

  // These are optional optimization methods.
  virtual void FindShortestSeparator(std::string *, const Slice &) const override { }
  virtual void FindShortSuccessor(std::string *) const override { }

private:
  val m_jsCompare = {};
  std::string m_name = "";
};

// ----------------------------------------------------------------------------
// Compressors
// ----------------------------------------------------------------------------

// Slot zero selects the compressor used for new blocks. A pass-through
// compressor lets us select uncompressed output while keeping the zlib
// decompressors packed into subsequent slots for reading existing MCBE data.
class NoCompression: public Compressor {
public:
  NoCompression(): Compressor(0) { }

  virtual void compressImpl(const char *input, size_t length, std::string &output) const override {
    output.assign(input, length);
  }

  virtual bool decompress(const char *input, size_t length, std::string &output) const override {
    output.assign(input, length);
    return true;
  }
};

// ----------------------------------------------------------------------------
// Options Wrappers
// ----------------------------------------------------------------------------

class OptionsWrapper {
public:
  ~OptionsWrapper() = default;
  OptionsWrapper() {
    m_options.create_if_missing = true;
    m_none = emldb::make_unique<NoCompression>();
    m_zlibRaw = emldb::make_unique<ZlibCompressorRaw>(-1);
    m_zlib = emldb::make_unique<ZlibCompressor>(-1);
    // Default to raw-zlib (ID 4), the compression MCBE itself writes.
    m_installCompressors(m_zlibRaw.get());
  }
  OptionsWrapper(const OptionsWrapper &) = delete;
  OptionsWrapper &operator=(const OptionsWrapper &) = delete;

  Options &get() { return m_options; }

  void setCreateIfMissing(bool value) { m_options.create_if_missing = value; }
  void setErrorIfExists(bool value) { m_options.error_if_exists = value; }
  void setParanoidChecks(bool value) { m_options.paranoid_checks = value; }
  void setWriteBufferSize(size_t size) {
    if (size == 0) throwJsError("write buffer size must be positive");
    m_options.write_buffer_size = size;
  }
  void setMaxOpenFiles(int num) {
    if (num <= 0) throwJsError("max open files must be positive");
    m_options.max_open_files = num;
  }
  void setBlockSize(size_t size) {
    if (size == 0) throwJsError("block size must be positive");
    m_options.block_size = size;
  }
  void setBlockRestartInterval(int interval) {
    if (interval <= 0) throwJsError("block restart interval must be positive");
    m_options.block_restart_interval = interval;
  }
  void setMaxFileSize(size_t size) {
    if (size == 0) throwJsError("max file size must be positive");
    m_options.max_file_size = size;
  }
  void setNoCompression() { m_installCompressors(m_none.get()); }
  void setZlibCompression(int level) {
    m_validateCompressionLevel(level);
    m_zlib = emldb::make_unique<ZlibCompressor>(level);
    m_installCompressors(m_zlib.get());
  }
  void setZlibRawCompression(int level) {
    m_validateCompressionLevel(level);
    m_zlibRaw = emldb::make_unique<ZlibCompressorRaw>(level);
    m_installCompressors(m_zlibRaw.get());
  }
  // ZSTD is compiled only with -DZSTD (requires libzstd port); kept as a stub
  // so the JS API can advertise it without a runtime cost.
  void setZstdCompression(int /*level*/) {
    throwJsError("ZSTD compression is not enabled in this build (rebuild with -DZSTD)");
  }
  void setComparator(val jsFunc, const std::string &name) { m_comparator = emldb::make_unique<JSComparator>(jsFunc, name); m_options.comparator = m_comparator.get(); }
  void setCache(size_t capacity) {
    if (capacity == 0) throwJsError("cache capacity must be positive");
    m_cache.reset(NewLRUCache(capacity));
    m_options.block_cache = m_cache.get();
  }
  void setBloomFilter(int bitsPerKey) {
    if (bitsPerKey <= 0) throwJsError("bloom filter bits must be positive");
    m_filterPolicy.reset(NewBloomFilterPolicy(bitsPerKey));
    m_options.filter_policy = m_filterPolicy.get();
  }

private:
  // Compressors have to be packed from slot 0 with no gaps in between:
  //   * table_builder.cc always writes blocks with compressors[0], so that slot
  //     selects the compression used for new blocks;
  //   * table/format.cc scans the slots in order and stops at the first empty
  //     one (`if (!c || c->uniqueCompressionID == compressionID)`), so a hole
  //     silently disables every compressor placed after it.
  void m_installCompressors(Compressor *writer) {
    Compressor *wanted[] = {writer, m_zlibRaw.get(), m_zlib.get()};
    const int wantedCount = sizeof(wanted) / sizeof(wanted[0]);

    for (int i = 0; i < 256; ++i) {
      m_options.compressors[i] = nullptr;
    }

    int used = 0;
    for (int i = 0; i < wantedCount; ++i) {
      bool duplicate = false;
      for (int j = 0; j < used; ++j) {
        if (m_options.compressors[j] == wanted[i]) duplicate = true;
      }
      if (!duplicate) m_options.compressors[used++] = wanted[i];
    }
  }

  void m_validateCompressionLevel(int level) {
    if (level < -1 || level > 9) {
      throwJsError("compression level must be between -1 and 9");
    }
  }

  Options m_options = {};
  std::unique_ptr<NoCompression> m_none = {};
  std::unique_ptr<ZlibCompressorRaw> m_zlibRaw = {};
  std::unique_ptr<ZlibCompressor> m_zlib = {};
  std::unique_ptr<JSComparator> m_comparator = {};
  std::unique_ptr<Cache> m_cache = {};
  std::unique_ptr<const FilterPolicy> m_filterPolicy = {};
};

class ReadOptionsWrapper {
public:
  ~ReadOptionsWrapper() = default;
  ReadOptionsWrapper() = default;
  ReadOptionsWrapper(const ReadOptionsWrapper &) = delete;
  ReadOptionsWrapper &operator=(const ReadOptionsWrapper &) = delete;

  ReadOptions &get() { return m_options; }

  void setVerifyChecksums(bool value) { m_options.verify_checksums = value; }
  void setFillCache(bool value) { m_options.fill_cache = value; }

private:
  ReadOptions m_options = {};
};

class WriteOptionsWrapper {
public:
  ~WriteOptionsWrapper() = default;
  WriteOptionsWrapper() = default;
  WriteOptionsWrapper(const WriteOptionsWrapper &) = delete;
  WriteOptionsWrapper &operator=(const WriteOptionsWrapper &) = delete;

  WriteOptions &get() { return m_options; }

  void setSync(bool value) { m_options.sync = value; }

private:
  WriteOptions m_options = {};
};

// ----------------------------------------------------------------------------
// Iterator Wrapper
// ----------------------------------------------------------------------------

class IteratorWrapper {
public:
  ~IteratorWrapper() = default;
  explicit IteratorWrapper(Iterator *iter) { m_iter.reset(iter); }
  IteratorWrapper(const IteratorWrapper &) = delete;
  IteratorWrapper &operator=(const IteratorWrapper &) = delete;

  std::string getError() const { Status status = m_iter->status(); return status.ok() ? "" : status.ToString(); }

  bool valid() const { return m_iter->Valid(); }
  void seekToFirst() { m_iter->SeekToFirst(); }
  void seekToLast() { m_iter->SeekToLast(); }

  void seek(const val &key) { m_iter->Seek(jsArrayToString(key)); }
  void next() { m_iter->Next(); }
  void prev() { m_iter->Prev(); }

  // Size-only accessors. db_bench's readseq/readreverse accumulate
  // key.size()+value.size() and never touch the bytes, so returning the length
  // as an integer lets the JS side run the same loop the C++ side runs. Going
  // through key()/value() would copy every record across the boundary just to
  // read a property off the resulting object.
  uint32_t keySize() const { return m_iter->Valid() ? static_cast<uint32_t>(m_iter->key().size()) : 0; }
  uint32_t valueSize() const { return m_iter->Valid() ? static_cast<uint32_t>(m_iter->value().size()) : 0; }

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

  // Zero-copy views. HEAPU8.subarray() aliases the wasm heap instead of
  // allocating, which is what makes a sequential scan O(1) allocations instead
  // of one per record. The returned view is only valid until the next iterator
  // mutation (next/prev/seek), because that is when the underlying block or
  // memtable node can be released or the heap can move; this is the same
  // contract as leveldb's Slice.
  val keyView() const {
    if (!m_iter->Valid()) return val::undefined();
    Slice k = m_iter->key();
    return emJsView(k.data(), k.size());
  }

  val valueView() const {
    if (!m_iter->Valid()) return val::undefined();
    Slice v = m_iter->value();
    return emJsView(v.data(), v.size());
  }

private:
  std::unique_ptr<Iterator> m_iter = {};
};

// ----------------------------------------------------------------------------
// WriteBatchWrapper
// ----------------------------------------------------------------------------

class WriteBatchWrapper {
public:
  ~WriteBatchWrapper() = default;
  WriteBatchWrapper() = default;
  WriteBatchWrapper(const WriteBatchWrapper &) = delete;
  WriteBatchWrapper &operator=(const WriteBatchWrapper &) = delete;

  WriteBatch &get() { return m_batch; }

  void put(const val &key, const val &value) { m_batch.Put(jsArrayToString(key), jsArrayToString(value)); }
  void del(const val &key) { m_batch.Delete(jsArrayToString(key)); }
  void clear() { m_batch.Clear(); }

private:
  WriteBatch m_batch = {};
};

// ----------------------------------------------------------------------------
// LevelDBWrapper
// ----------------------------------------------------------------------------

class LevelDBWrapper {
public:
  static void destroy(const std::string &path, OptionsWrapper &options) {
    Status status = DestroyDB(path, options.get());
    if (!status.ok()) {
      throwJsError(status.ToString());
    }
  }

  static void repair(const std::string &path, OptionsWrapper &options) {
    Status status = RepairDB(path, options.get());
    if (!status.ok()) {
      throwJsError(status.ToString());
    }
  }

  ~LevelDBWrapper() = default;
  LevelDBWrapper() = default;
  LevelDBWrapper(const LevelDBWrapper &) = delete;
  LevelDBWrapper &operator=(const LevelDBWrapper &) = delete;

  void open(
    const std::string &path,
    OptionsWrapper &options
  ) {
    m_path = path;
    DB *db = nullptr;
    Status status = DB::Open(options.get(), path, &db);

    if (!status.ok()) {
      throwJsError(status.ToString());
    }

    m_db.reset(db);
  }

  void close() { m_db.reset(); }

  void put(const val &key, const val &value, WriteOptionsWrapper &options) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    std::string key_str = jsArrayToString(key);
    std::string value_str = jsArrayToString(value);

    Status status = m_db->Put(options.get(), key_str, value_str);
    if (!status.ok()) {
      throwJsError(status.ToString());
    }
  }

  val get(const val &key, ReadOptionsWrapper &options) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    std::string key_str = jsArrayToString(key);
    std::string value_str;

    Status status = m_db->Get(options.get(), key_str, &value_str);

    if (status.IsNotFound()) {
      return val::undefined();
    }

    if (!status.ok()) {
      throwJsError(status.ToString());
    }

    return stringToJsArray(value_str);
  }

  // Zero-copy variant: the value is read into this wrapper's scratch buffer and
  // returned as a view over it, so consecutive getView calls reuse one
  // allocation instead of one per read. The view is invalidated by the next
  // getView (or any other read) on this DB.
  val getView(const val &key, ReadOptionsWrapper &options) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    std::string key_str = jsArrayToString(key);
    m_scratch.clear();

    Status status = m_db->Get(options.get(), key_str, &m_scratch);
    if (status.IsNotFound()) {
      return val::undefined();
    }
    if (!status.ok()) {
      throwJsError(status.ToString());
    }

    return emJsView(m_scratch.data(), m_scratch.size());
  }

  void del(const val &key, WriteOptionsWrapper &options) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    std::string key_str = jsArrayToString(key);

    Status status = m_db->Delete(options.get(), key_str);
    if (!status.ok()) {
      throwJsError(status.ToString());
    }
  }

  void write(WriteBatchWrapper &batch, WriteOptionsWrapper &options) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    Status status = m_db->Write(options.get(), &batch.get());
    if (!status.ok()) {
      throwJsError(status.ToString());
    }
  }

  IteratorWrapper *createIterator(ReadOptionsWrapper &options) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    Iterator *iter = m_db->NewIterator(options.get());
    return new IteratorWrapper(iter);
  }

  void compactRange(const val &start_key, const val &limit_key) {
    if (!m_db) {
      throwJsError("Database is not open");
    }

    std::string start_str, limit_str;
    Slice start_slice, limit_slice;
    Slice *start = nullptr;
    Slice *limit = nullptr;

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
      throwJsError("Database is not open");
    }

    std::string value;
    if (m_db->GetProperty(property, &value)) {
      return value;
    }
    return "";
  }

private:
  std::unique_ptr<DB> m_db;
  std::string m_path = "";
  // Backing store for getView(); reused across calls so a scan does not
  // allocate per read.
  std::string m_scratch = {};
};

}  // namespace leveldb

// ----------------------------------------------------------------------------
// Emscripten Bindings
// ----------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(leveldb) {
  using namespace leveldb;

#if defined(LEVELDB_PLATFORM_EMSCRIPTEN)
  // Environment setup
  function("SetFS", &SetFS);
  function("SetFileWriteBuffer", &SetFileWriteBuffer);
#endif

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
    .function("setMaxFileSize", &OptionsWrapper::setMaxFileSize)
    .function("setNoCompression", &OptionsWrapper::setNoCompression)
    .function("setZlibCompression", &OptionsWrapper::setZlibCompression)
    .function("setZlibRawCompression", &OptionsWrapper::setZlibRawCompression)
    .function("setZstdCompression", &OptionsWrapper::setZstdCompression)
    .function("setComparator", &OptionsWrapper::setComparator)
    .function("setCache", &OptionsWrapper::setCache)
    .function("setBloomFilter", &OptionsWrapper::setBloomFilter);

  class_<ReadOptionsWrapper>("ReadOptions")
    .constructor<>()
    .function("setVerifyChecksums", &ReadOptionsWrapper::setVerifyChecksums)
    .function("setFillCache", &ReadOptionsWrapper::setFillCache);

  class_<WriteOptionsWrapper>("WriteOptions")
    .constructor<>()
    .function("setSync", &WriteOptionsWrapper::setSync);

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
    .function("keySize", &IteratorWrapper::keySize)
    .function("valueSize", &IteratorWrapper::valueSize)
    .function("keyView", &IteratorWrapper::keyView)
    .function("valueView", &IteratorWrapper::valueView)
    .function("getError", &IteratorWrapper::getError);

  // WriteBatch
  class_<WriteBatchWrapper>("WriteBatch")
    .constructor<>()
    .function("put", &WriteBatchWrapper::put)
    .function("del", &WriteBatchWrapper::del)
    .function("clear", &WriteBatchWrapper::clear);

  // Database
  class_<LevelDBWrapper>("DB")
    .constructor<>()
    .function("open", &LevelDBWrapper::open)
    .function("close", &LevelDBWrapper::close)
    .function("put", &LevelDBWrapper::put)
    .function("get", &LevelDBWrapper::get)
    .function("getView", &LevelDBWrapper::getView)
    .function("del", &LevelDBWrapper::del)
    .function("write", &LevelDBWrapper::write)
    .function("createIterator", &LevelDBWrapper::createIterator, allow_raw_pointers())
    .function("compactRange", &LevelDBWrapper::compactRange)
    .function("getProperty", &LevelDBWrapper::getProperty)
    .class_function("destroy", &LevelDBWrapper::destroy)
    .class_function("repair", &LevelDBWrapper::repair);
}
