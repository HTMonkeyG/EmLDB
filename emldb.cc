// emldb.cc - High-performance Emscripten bindings for LevelDB using Embind
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

// C++11 compatible make_unique
namespace std {
    template<typename T, typename... Args>
    unique_ptr<T> make_unique(Args&&... args) {
        return unique_ptr<T>(new T(std::forward<Args>(args)...));
    }
}

// ============================================================================
// [SECTION] Utility Functions
// ============================================================================

// Convert JavaScript Uint8Array to std::string (binary data)
std::string jsArrayToString(const val& jsArray) {
    unsigned int length = jsArray["length"].as<unsigned int>();
    std::string result;
    result.reserve(length);

    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(static_cast<char>(jsArray[i].as<unsigned char>()));
    }
    return result;
}

// Convert std::string to JavaScript Uint8Array
val stringToJsArray(const std::string& str) {
    val uint8Array = val::global("Uint8Array").new_(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        uint8Array.set(i, static_cast<unsigned char>(str[i]));
    }
    return uint8Array;
}

// ============================================================================
// [SECTION] Custom Comparator Wrapper
// ============================================================================

class JSComparator : public leveldb::Comparator {
private:
    val jsCompare_;
    std::string name_;

public:
    JSComparator(val jsCompare, const std::string& name)
        : jsCompare_(jsCompare), name_(name) {}

    int Compare(const leveldb::Slice& a, const leveldb::Slice& b) const override {
        val aArray = stringToJsArray(std::string(a.data(), a.size()));
        val bArray = stringToJsArray(std::string(b.data(), b.size()));
        return jsCompare_(aArray, bArray).as<int>();
    }

    const char* Name() const override {
        return name_.c_str();
    }

    void FindShortestSeparator(std::string*, const leveldb::Slice&) const override {}
    void FindShortSuccessor(std::string*) const override {}
};

// ============================================================================
// [SECTION] Options Wrappers
// ============================================================================

class OptionsWrapper {
private:
    leveldb::Options options_;
    std::unique_ptr<JSComparator> comparator_;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> filterPolicy_;
    std::unique_ptr<leveldb::ZlibCompressor> zlibCompressor_;
    std::unique_ptr<leveldb::ZlibCompressorRaw> zlibRawCompressor_;

public:
    OptionsWrapper() {
        options_.create_if_missing = true;
        // Initialize compressor array to NULL
        for (int i = 0; i < 256; ++i) {
            options_.compressors[i] = nullptr;
        }
    }

    void setCreateIfMissing(bool value) { options_.create_if_missing = value; }
    void setErrorIfExists(bool value) { options_.error_if_exists = value; }
    void setParanoidChecks(bool value) { options_.paranoid_checks = value; }
    void setWriteBufferSize(size_t size) { options_.write_buffer_size = size; }
    void setMaxOpenFiles(int num) { options_.max_open_files = num; }
    void setBlockSize(size_t size) { options_.block_size = size; }
    void setBlockRestartInterval(int interval) { options_.block_restart_interval = interval; }

    void setZlibCompression(int level) {
        zlibCompressor_ = std::make_unique<leveldb::ZlibCompressor>(level);
        options_.compressors[zlibCompressor_->uniqueCompressionID] = zlibCompressor_.get();
    }

    void setZlibRawCompression(int level) {
        zlibRawCompressor_ = std::make_unique<leveldb::ZlibCompressorRaw>(level);
        options_.compressors[zlibRawCompressor_->uniqueCompressionID] = zlibRawCompressor_.get();
    }

    void setComparator(val jsCompare, const std::string& name) {
        comparator_ = std::make_unique<JSComparator>(jsCompare, name);
        options_.comparator = comparator_.get();
    }

    void setCache(size_t capacity) {
        cache_ = std::unique_ptr<leveldb::Cache>(leveldb::NewLRUCache(capacity));
        options_.block_cache = cache_.get();
    }

    void setBloomFilter(int bitsPerKey) {
        filterPolicy_ = std::unique_ptr<const leveldb::FilterPolicy>(
            leveldb::NewBloomFilterPolicy(bitsPerKey)
        );
        options_.filter_policy = filterPolicy_.get();
    }

    leveldb::Options& get() { return options_; }
};

class ReadOptionsWrapper {
private:
    leveldb::ReadOptions options_;

public:
    ReadOptionsWrapper() = default;

    void setVerifyChecksums(bool value) { options_.verify_checksums = value; }
    void setFillCache(bool value) { options_.fill_cache = value; }

    leveldb::ReadOptions& get() { return options_; }
};

class WriteOptionsWrapper {
private:
    leveldb::WriteOptions options_;

public:
    WriteOptionsWrapper() = default;

    void setSync(bool value) { options_.sync = value; }

    leveldb::WriteOptions& get() { return options_; }
};

// ============================================================================
// [SECTION] Iterator Wrapper
// ============================================================================

class IteratorWrapper {
private:
    std::unique_ptr<leveldb::Iterator> iter_;

public:
    IteratorWrapper(leveldb::Iterator* iter) : iter_(iter) {}

    bool valid() const {
        return iter_->Valid();
    }

    void seekToFirst() {
        iter_->SeekToFirst();
    }

    void seekToLast() {
        iter_->SeekToLast();
    }

    void seek(const val& key) {
        std::string keyStr = jsArrayToString(key);
        iter_->Seek(keyStr);
    }

    void next() {
        iter_->Next();
    }

    void prev() {
        iter_->Prev();
    }

    val key() const {
        if (!iter_->Valid()) {
            return val::undefined();
        }
        leveldb::Slice k = iter_->key();
        return stringToJsArray(std::string(k.data(), k.size()));
    }

    val value() const {
        if (!iter_->Valid()) {
            return val::undefined();
        }
        leveldb::Slice v = iter_->value();
        return stringToJsArray(std::string(v.data(), v.size()));
    }

    std::string getError() const {
        leveldb::Status status = iter_->status();
        if (!status.ok()) {
            return status.ToString();
        }
        return "";
    }
};

// ============================================================================
// [SECTION] WriteBatch Wrapper
// ============================================================================

class WriteBatchWrapper {
private:
    leveldb::WriteBatch batch_;

public:
    WriteBatchWrapper() = default;

    void put(const val& key, const val& value) {
        std::string keyStr = jsArrayToString(key);
        std::string valueStr = jsArrayToString(value);
        batch_.Put(keyStr, valueStr);
    }

    void del(const val& key) {
        std::string keyStr = jsArrayToString(key);
        batch_.Delete(keyStr);
    }

    void clear() {
        batch_.Clear();
    }

    leveldb::WriteBatch& get() { return batch_; }
};

// ============================================================================
// [SECTION] Main Database Wrapper
// ============================================================================

class LevelDBWrapper {
private:
    std::unique_ptr<leveldb::DB> db_;
    std::string path_;

public:
    LevelDBWrapper() = default;

    void open(const std::string& path, OptionsWrapper& options) {
        path_ = path;
        leveldb::DB* db = nullptr;
        leveldb::Status status = leveldb::DB::Open(options.get(), path, &db);

        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }

        db_.reset(db);
    }

    void close() {
        db_.reset();
    }

    void put(const val& key, const val& value, WriteOptionsWrapper& options) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        std::string keyStr = jsArrayToString(key);
        std::string valueStr = jsArrayToString(value);

        leveldb::Status status = db_->Put(options.get(), keyStr, valueStr);
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    val get(const val& key, ReadOptionsWrapper& options) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        std::string keyStr = jsArrayToString(key);
        std::string valueStr;

        leveldb::Status status = db_->Get(options.get(), keyStr, &valueStr);

        if (status.IsNotFound()) {
            return val::undefined();
        }

        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }

        return stringToJsArray(valueStr);
    }

    void del(const val& key, WriteOptionsWrapper& options) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        std::string keyStr = jsArrayToString(key);

        leveldb::Status status = db_->Delete(options.get(), keyStr);
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    void write(WriteBatchWrapper& batch, WriteOptionsWrapper& options) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        leveldb::Status status = db_->Write(options.get(), &batch.get());
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    IteratorWrapper* createIterator(ReadOptionsWrapper& options) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        leveldb::Iterator* iter = db_->NewIterator(options.get());
        return new IteratorWrapper(iter);
    }

    void compactRange(const val& startKey, const val& limitKey) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        std::string startStr, limitStr;
        leveldb::Slice startSlice, limitSlice;
        leveldb::Slice* start = nullptr;
        leveldb::Slice* limit = nullptr;

        if (!startKey.isUndefined() && !startKey.isNull()) {
            startStr = jsArrayToString(startKey);
            startSlice = leveldb::Slice(startStr);
            start = &startSlice;
        }

        if (!limitKey.isUndefined() && !limitKey.isNull()) {
            limitStr = jsArrayToString(limitKey);
            limitSlice = leveldb::Slice(limitStr);
            limit = &limitSlice;
        }

        db_->CompactRange(start, limit);
    }

    std::string getProperty(const std::string& property) {
        if (!db_) {
            throw std::runtime_error("Database is not open");
        }

        std::string value;
        if (db_->GetProperty(property, &value)) {
            return value;
        }
        return "";
    }

    static void destroy(const std::string& path, OptionsWrapper& options) {
        leveldb::Status status = leveldb::DestroyDB(path, options.get());
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    static void repair(const std::string& path, OptionsWrapper& options) {
        leveldb::Status status = leveldb::RepairDB(path, options.get());
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }
};

// ============================================================================
// [SECTION] Embind Bindings
// ============================================================================

EMSCRIPTEN_BINDINGS(leveldb) {
    // Options
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
        .function("del", &LevelDBWrapper::del)
        .function("write", &LevelDBWrapper::write)
        .function("createIterator", &LevelDBWrapper::createIterator, allow_raw_pointers())
        .function("compactRange", &LevelDBWrapper::compactRange)
        .function("getProperty", &LevelDBWrapper::getProperty)
        .class_function("destroy", &LevelDBWrapper::destroy)
        .class_function("repair", &LevelDBWrapper::repair);
}
