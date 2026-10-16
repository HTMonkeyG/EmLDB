var EmLDB = null;
var EmLDBPromise = null;
var EmLDBMemoryProfile = null;

function resolveModule(profile) {
  return profile === "scalable"
    ? require("../dist/libleveldb-growth.js")
    : require("../dist/libleveldb.js");
}

function normalizeInitializationOptions(options) {
  if (options == null) options = {};
  if (typeof options !== "object" || Array.isArray(options))
    throw new TypeError("initialization options must be an object");
  const memory = options.memory == null ? "performance" : options.memory;
  if (memory !== "performance" && memory !== "scalable")
    throw new RangeError('memory must be "performance" or "scalable"');
  return memory;
}

// Check whether EmLDB is set.
function GUARD_checkEmLDB() {
  if (!EmLDB)
    throw new Error("LevelDB not initialized.");
}

// Call with stack restore.
// WARN: The function is slower than raw try-finally-restore. DO NOT use in hot
//       paths.
function GUARD_safeCall(emfunc, ...args) {
  const sp = EmLDB.stackSave();
  try {
    emfunc(...args);
  } catch (e) {
    EmLDB.stackRestore(sp);
  }
}

function GUARD_getException(e) {
  if (typeof e === "number" || e instanceof WebAssembly.Exception)
    return new Error(EmLDB.getExceptionMessage(e));
  return e;
}

// ----------------------------------------------------------------------------
// [SECTION] UTILS
// ----------------------------------------------------------------------------

function createGCListener(callback) {
  return typeof FinalizationRegistry !== "undefined" ? new FinalizationRegistry(callback) : {
    register: () => { },
    unregister: () => { }
  };
}

function toUint8Array(data) {
  if (data instanceof Uint8Array)
    return data;
  if (typeof data === "string")
    return new TextEncoder().encode(data);
  throw new Error("Data must be a string or Uint8Array");
}

function assertBoolean(value, name) {
  if (typeof value !== "boolean")
    throw new TypeError(`${name} must be a boolean`);
}

function assertInteger(value, name, min, max) {
  if (!Number.isSafeInteger(value) || value < min || value > max)
    throw new RangeError(`${name} must be an integer between ${min} and ${max}`);
}

function assertPath(path) {
  if (typeof path !== "string" || path.length === 0)
    throw new TypeError("path must be a non-empty string");
  if (path.includes("\0"))
    throw new TypeError("path must not contain NUL characters");
}

function releaseDBState(state) {
  if (!state)
    return;

  if (state.db) {
    try { state.db.close(); } catch (_) { }
    try { state.db.delete(); } catch (_) { }
  }

  for (const name of ["opt", "defaultReadOpt", "defaultWriteOpt"]) {
    try {
      if (state[name]) state[name].free();
    } catch (_) { }
  }
}

// ----------------------------------------------------------------------------
// [SECTION] OPTIONS
// ----------------------------------------------------------------------------

class LevelDBOptions {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(options = {}) {
    if (options == null) options = {};
    if (typeof options !== "object" || Array.isArray(options))
      throw new TypeError("options must be an object");

    GUARD_checkEmLDB();

    this.opt = new EmLDB.Options();
    LevelDBOptions.GC.register(this, this.opt, this);

    const sp = EmLDB.stackSave();
    try {
      // Apply options
      if (options.createIfMissing != null) {
        assertBoolean(options.createIfMissing, "createIfMissing");
        this.opt.setCreateIfMissing(options.createIfMissing);
      }
      if (options.errorIfExists != null) {
        assertBoolean(options.errorIfExists, "errorIfExists");
        this.opt.setErrorIfExists(options.errorIfExists);
      }
      if (options.paranoidChecks != null) {
        assertBoolean(options.paranoidChecks, "paranoidChecks");
        this.opt.setParanoidChecks(options.paranoidChecks);
      }
      if (options.writeBufferSize != null) {
        assertInteger(options.writeBufferSize, "writeBufferSize", 1, 0xffffffff);
        this.opt.setWriteBufferSize(options.writeBufferSize);
      }
      if (options.maxOpenFiles != null) {
        assertInteger(options.maxOpenFiles, "maxOpenFiles", 1, 0x7fffffff);
        this.opt.setMaxOpenFiles(options.maxOpenFiles);
      }
      if (options.blockSize != null) {
        assertInteger(options.blockSize, "blockSize", 1, 0xffffffff);
        this.opt.setBlockSize(options.blockSize);
      }
      if (options.blockRestartInterval != null) {
        assertInteger(options.blockRestartInterval, "blockRestartInterval", 1, 0x7fffffff);
        this.opt.setBlockRestartInterval(options.blockRestartInterval);
      }
      if (options.cacheSize != null) {
        assertInteger(options.cacheSize, "cacheSize", 0, 0xffffffff);
        if (options.cacheSize > 0)
          this.opt.setCache(options.cacheSize);
      }
      if (options.bloomFilterBits != null) {
        assertInteger(options.bloomFilterBits, "bloomFilterBits", 0, 0x7fffffff);
        if (options.bloomFilterBits > 0)
          this.opt.setBloomFilter(options.bloomFilterBits);
      }

      if (options.compression != null) {
        const level = options.compressionLevel == null ? -1 : options.compressionLevel;
        assertInteger(level, "compressionLevel", -1, 9);

        switch (options.compression) {
          case LevelDB.COMPRESSIONS.NONE:
            if (options.compressionLevel != null)
              throw new Error("compressionLevel is not valid with NONE compression");
            this.opt.setNoCompression();
            break;
          case LevelDB.COMPRESSIONS.ZLIB:
            this.opt.setZlibCompression(level);
            break;
          case LevelDB.COMPRESSIONS.ZLIB_RAW:
            this.opt.setZlibRawCompression(level);
            break;
          case LevelDB.COMPRESSIONS.SNAPPY:
            throw new Error("SNAPPY compression is not included in this build");
          default:
            throw new RangeError("compression must be one of LevelDB.COMPRESSIONS");
        }
      } else if (options.compressionLevel != null) {
        throw new Error("compressionLevel requires an explicit compression option");
      }

      if (options.mcneEncryptionKey != null) {
        const key = toUint8Array(options.mcneEncryptionKey);
        if (key.byteLength !== 8)
          throw new Error("mcneEncryptionKey must be 8 bytes length");
        this.opt.setMcneWrapper(true, key);
      } else {
        this.opt.setMcneWrapper(false, "");
      }

      // A JS function is represented by an emscripten::val owned by the main
      // runtime thread. LevelDB also invokes comparators from its compaction
      // worker, where that handle is invalid. Reject the unsafe configuration.
      if (options.comparator != null)
        throw new Error("JavaScript comparators are not supported by the threaded build");
    } catch (error) {
      EmLDB.stackRestore(sp);
      this.free();
      throw GUARD_getException(error);
    }
  }

  get() {
    return this.opt;
  }

  free() {
    if (!this.opt)
      return;

    LevelDBOptions.GC.unregister(this);
    const opt = this.opt;
    this.opt = null;
    opt.delete();
  }
}

class LevelDBReadOptions {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(options = {}) {
    if (options == null) options = {};
    if (typeof options !== "object" || Array.isArray(options))
      throw new TypeError("read options must be an object");

    GUARD_checkEmLDB();

    this.opt = new EmLDB.ReadOptions();
    LevelDBReadOptions.GC.register(this, this.opt, this);

    const sp = EmLDB.stackSave();
    try {
      if (options.verifyChecksums != null) {
        assertBoolean(options.verifyChecksums, "verifyChecksums");
        this.opt.setVerifyChecksums(options.verifyChecksums);
      }
      if (options.fillCache != null) {
        assertBoolean(options.fillCache, "fillCache");
        this.opt.setFillCache(options.fillCache);
      }
    } catch (error) {
      EmLDB.stackRestore(sp);
      this.free();
      throw GUARD_getException(error);
    }
  }

  get() {
    return this.opt;
  }

  free() {
    if (!this.opt)
      return;

    LevelDBReadOptions.GC.unregister(this);
    const opt = this.opt;
    this.opt = null;
    opt.delete();
  }
}

class LevelDBWriteOptions {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(options = {}) {
    if (options == null) options = {};
    if (typeof options !== "object" || Array.isArray(options))
      throw new TypeError("write options must be an object");
    GUARD_checkEmLDB();

    this.opt = new EmLDB.WriteOptions();
    LevelDBWriteOptions.GC.register(this, this.opt, this);

    const sp = EmLDB.stackSave();
    try {
      if (options.sync != null) {
        assertBoolean(options.sync, "sync");
        this.opt.setSync(options.sync);
      }
    } catch (error) {
      EmLDB.stackRestore(sp);
      this.free();
      throw GUARD_getException(error);
    }
  }

  get() {
    return this.opt;
  }

  free() {
    if (!this.opt) return;
    LevelDBWriteOptions.GC.unregister(this);
    const opt = this.opt;
    this.opt = null;
    opt.delete();
  }
}

// ----------------------------------------------------------------------------
// [SECTION] WRITE BATCH
// ----------------------------------------------------------------------------

class LevelDBWriteBatch {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor() {
    GUARD_checkEmLDB();
    this.batch = new EmLDB.WriteBatch();
    LevelDBWriteBatch.GC.register(this, this.batch, this);
  }

  put(key, value) {
    this.validate();
    key = toUint8Array(key);
    value = toUint8Array(value);
    this.batch.put(key, value);
    return this;
  }

  del(key) {
    this.validate();
    key = toUint8Array(key);
    this.batch.del(key);
    return this;
  }

  clear() {
    this.validate();
    this.batch.clear();
    return this;
  }

  get() {
    this.validate();
    return this.batch;
  }

  validate() {
    if (!this.batch)
      throw new Error("try to access freed write batch.");
  }

  free() {
    if (!this.batch) return;
    LevelDBWriteBatch.GC.unregister(this);
    const batch = this.batch;
    this.batch = null;
    batch.delete();
  }
}

// ----------------------------------------------------------------------------
// [SECTION] ITERATOR
// ----------------------------------------------------------------------------

class LevelDBIteratorBase {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(iter) {
    this.iter = iter;
    LevelDBIteratorBase.GC.register(this, this.iter, this);
  }

  validate() {
    if (!this.iter)
      throw new Error("try to access freed iterator.");
  }

  valid() {
    if (!this.iter)
      return false;
    return this.iter.valid();
  }

  free() {
    if (!this.iter)
      return;

    if (this.owner)
      this.owner.releaseDerivative(this);
    LevelDBIteratorBase.GC.unregister(this);
    const iter = this.iter;
    this.iter = null;
    this.owner = null;
    iter.delete();
  }

  seekToFirst() {
    this.validate();
    this.iter.seekToFirst();
  }

  seekToLast() {
    this.validate();
    this.iter.seekToLast();
  }

  seek(key) {
    this.validate();
    key = toUint8Array(key);
    this.iter.seek(key);
  }

  next() {
    this.validate();
    this.iter.next();
  }

  prev() {
    this.validate();
    this.iter.prev();
  }

  key() {
    this.validate();
    return this.iter.key();
  }

  value() {
    this.validate();
    return this.iter.value();
  }

  getError() {
    this.validate();
    var err = this.iter.getError();
    if (err)
      throw new Error(err);
  }
}

class LevelDBIterator {
  constructor(base) {
    this.base = base;
    try {
      this.base.seekToFirst();
    } catch (error) {
      this.base.free();
      this.base = null;
      throw error;
    }
  }

  next() {
    if (!this.base)
      return { done: true, value: undefined };

    const base = this.base;
    try {
      if (!base.valid()) {
        base.getError();
        base.free();
        this.base = null;
        return { done: true, value: undefined };
      }

      const key = base.key();
      const value = base.value();
      base.next();

      if (!base.valid()) {
        base.getError();
        base.free();
        this.base = null;
      }

      return { value: [key, value], done: false };
    } catch (error) {
      try { base.free(); } catch (_) { }
      this.base = null;
      throw error;
    }
  }

  return() {
    if (this.base) {
      this.base.free();
      this.base = null;
    }
    return { done: true, value: undefined };
  }

  [Symbol.iterator]() {
    return this;
  }
}

class LevelDBKeyIterator extends LevelDBIterator {
  constructor(base) {
    super(base);
  }

  next() {
    var result = super.next();
    if (!result.done)
      result.value = result.value[0];
    return result;
  }
}

class LevelDBValueIterator extends LevelDBIterator {
  constructor(base) {
    super(base);
  }

  next() {
    var result = super.next();
    if (!result.done)
      result.value = result.value[1];
    return result;
  }
}

// ----------------------------------------------------------------------------
// [SECTION] LDB_MAIN
// ----------------------------------------------------------------------------

class LevelDB {
  static GC = createGCListener(releaseDBState);

  static COMPRESSIONS = {
    NONE: 0,
    SNAPPY: 1,
    ZLIB: 2,
    ZLIB_RAW: 4
  };

  static async initialize(options) {
    const memory = normalizeInitializationOptions(options);
    if (EmLDBMemoryProfile && EmLDBMemoryProfile !== memory)
      throw new Error(`LevelDB is already initialized with the ${EmLDBMemoryProfile} memory profile`);

    if (!EmLDBPromise) {
      EmLDBMemoryProfile = memory;
      EmLDBPromise = resolveModule(memory)().then(function (module) {
        EmLDB = module;
        return true;
      }).catch(function (error) {
        EmLDBPromise = null;
        EmLDBMemoryProfile = null;
        throw error;
      });
    }
    return EmLDBPromise;
  }

  static get memoryProfile() {
    return EmLDBMemoryProfile;
  }

  static destroy(path, options) {
    GUARD_checkEmLDB();
    assertPath(path);

    var opt = new LevelDBOptions(options);

    const sp = EmLDB.stackSave();
    try {
      EmLDB.DB.destroy(path, opt.get());
    } finally {
      EmLDB.stackRestore(sp);
      opt.free();
    }
  }

  static repair(path, options) {
    GUARD_checkEmLDB();
    assertPath(path);

    var opt = new LevelDBOptions(options);

    const sp = EmLDB.stackSave();
    try {
      EmLDB.DB.repair(path, opt.get());
    } finally {
      EmLDB.stackRestore(sp);
      opt.free();
    }
  }

  static infer(path, options) {
    GUARD_checkEmLDB();
    assertPath(path);

    var opt = new LevelDBOptions(options)
      , result = void 0;

    const sp = EmLDB.stackSave();
    try {
      result = EmLDB.DB.infer(path, opt.get());
    } finally {
      EmLDB.stackRestore(sp);
      opt.free();
      return result;
    }
  }

  static encrypt(path, options) {
    GUARD_checkEmLDB();
    assertPath(path);

    var opt = new LevelDBOptions(options);

    const sp = EmLDB.stackSave();
    try {
      EmLDB.DB.encrypt(path, opt.get());
    } finally {
      EmLDB.stackRestore(sp);
      opt.free();
    }
  }

  constructor(path, options) {
    GUARD_checkEmLDB();
    assertPath(path);
    if (options != null && (typeof options !== "object" || Array.isArray(options)))
      throw new TypeError("options must be an object");

    this.path = path;
    this.options = options || {};
    this.db = null;
    this.opt = null;
    this.derivatives = new Set();
    this.derivativeGC = createGCListener((ref) => this.derivatives.delete(ref));
    this.defaultWriteOpt = null;
    this.defaultReadOpt = null;
  }

  validate() {
    if (!this.db)
      throw new Error("try to access DB before it's open.");
  }

  releaseDerivative(iterator) {
    if (iterator.derivativeRef)
      this.derivatives.delete(iterator.derivativeRef);
    this.derivativeGC.unregister(iterator);
    iterator.derivativeRef = null;
  }

  open() {
    if (this.db)
      throw new Error("try to open DB that is already open.");

    var opt = null;
    var db = null;
    var defaultWriteOpt = null;
    var defaultReadOpt = null;

    const sp = EmLDB.stackSave();
    try {
      opt = new LevelDBOptions(this.options);
      db = new EmLDB.DB();
      defaultWriteOpt = new LevelDBWriteOptions();
      defaultReadOpt = new LevelDBReadOptions();
      db.open(this.path, opt.get());
    } catch (e) {
      EmLDB.stackRestore(sp);
      try { if (db) db.delete(); } catch (_) { }
      try { if (opt) opt.free(); } catch (_) { }
      try { if (defaultWriteOpt) defaultWriteOpt.free(); } catch (_) { }
      try { if (defaultReadOpt) defaultReadOpt.free(); } catch (_) { }
      throw GUARD_getException(e);
    }

    // The db keeps raw pointers to what the options own (block cache, filter
    // policy, comparator), so the options have to outlive it - see close().
    this.opt = opt;
    this.db = db;
    this.defaultWriteOpt = defaultWriteOpt;
    this.defaultReadOpt = defaultReadOpt;
    LevelDB.GC.register(this, {
      db,
      opt,
      defaultWriteOpt,
      defaultReadOpt
    }, this);
    return true;
  }

  close() {
    if (!this.db)
      throw new Error("try to close DB before it's open.");

    // Free all allocated objects before closing the db
    for (var d of this.derivatives) {
      var obj = d.deref();
      if (!obj)
        continue;
      try {
        this.derivativeGC.unregister(obj);
        obj.free();
      } catch (e) {
        // Iterator might already be freed, ignore
      }
    }

    LevelDB.GC.unregister(this);
    var closeError = null;
    try { this.db.close(); } catch (e) { closeError = e; }
    try { this.db.delete(); } catch (e) { if (!closeError) closeError = e; }
    this.db = null;
    this.derivatives.clear();

    // Only now may the options (and the cache / filter policy / comparator they
    // own) be released: the db was referencing them all along.
    if (this.opt) {
      this.opt.free();
      this.opt = null;
    }

    if (this.defaultReadOpt) {
      this.defaultReadOpt.free();
      this.defaultReadOpt = null;
    }
    if (this.defaultWriteOpt) {
      this.defaultWriteOpt.free();
      this.defaultWriteOpt = null;
    }

    if (closeError)
      throw closeError;

    return true;
  }

  put(key, value, options) {
    this.validate();

    key = toUint8Array(key);
    value = toUint8Array(value);
    var wopt = options ? new LevelDBWriteOptions(options) : this.defaultWriteOpt;

    const sp = EmLDB.stackSave();
    try {
      this.db.put(key, value, wopt.get());
      return true;
    } finally {
      EmLDB.stackRestore(sp);
      if (wopt != this.defaultWriteOpt) wopt.free();
    }
  }

  delete(key, options) {
    this.validate();

    key = toUint8Array(key);
    var wopt = options ? new LevelDBWriteOptions(options) : this.defaultWriteOpt;

    const sp = EmLDB.stackSave();
    try {
      this.db.del(key, wopt.get());
      return true;
    } finally {
      EmLDB.stackRestore(sp);
      if (wopt != this.defaultWriteOpt) wopt.free();
      return false;
    }
  }

  write(batch, options) {
    this.validate();
    if (!(batch instanceof LevelDBWriteBatch))
      throw new TypeError("batch must be a LevelDBWriteBatch");

    var wopt = options ? new LevelDBWriteOptions(options) : this.defaultWriteOpt;

    const sp = EmLDB.stackSave();
    try {
      this.db.write(batch.get(), wopt.get());
      return true;
    } finally {
      EmLDB.stackRestore(sp);
      if (wopt != this.defaultWriteOpt) wopt.free();
      return false;
    }
  }

  batch(operations) {
    var batch = new LevelDBWriteBatch();

    const sp = EmLDB.stackSave();
    try {
      if (operations != null && !Array.isArray(operations))
        throw new TypeError("operations must be an array");

      if (Array.isArray(operations)) {
        for (var op of operations) {
          if (!op || typeof op !== "object")
            throw new TypeError("batch operations must be objects");
          if (op.type === "put") {
            if (op.value == null)
              throw new TypeError("put operations require a value");
            batch.put(op.key, op.value);
          } else if (op.type === "del") {
            batch.del(op.key);
          } else {
            throw new Error(`unknown batch operation type: ${op.type}`);
          }
        }
      }
    } catch (error) {
      EmLDB.stackRestore(sp);
      batch.free();
      throw GUARD_getException(error);
    }

    return batch;
  }

  get(key, options) {
    this.validate();

    key = toUint8Array(key);
    var ropt = options ? new LevelDBReadOptions(options) : this.defaultReadOpt;

    const sp = EmLDB.stackSave();
    try {
      return this.db.get(key, ropt.get());
    } finally {
      EmLDB.stackRestore(sp);
      if (ropt != this.defaultReadOpt) ropt.free();
    }
  }

  compact(startKey, limitKey) {
    this.validate();

    if (startKey !== undefined && startKey !== null)
      startKey = toUint8Array(startKey);
    if (limitKey !== undefined && limitKey !== null)
      limitKey = toUint8Array(limitKey);

    this.db.compactRange(startKey, limitKey);
  }

  getProperty(property) {
    this.validate();
    if (typeof property !== "string" || property.length === 0)
      throw new TypeError("property must be a non-empty string");
    return this.db.getProperty(property);
  }

  iterator(options) {
    this.validate();

    var ropt = options ? new LevelDBReadOptions(options) : this.defaultReadOpt;
    var iter;

    const sp = EmLDB.stackSave();
    try {
      iter = this.db.createIterator(ropt.get());
    } finally {
      EmLDB.stackRestore(sp);
      if (ropt != this.defaultReadOpt) ropt.free();
    }

    var result = new LevelDBIteratorBase(iter);

    // Track live iterators without retaining them. Closing the DB can then
    // release native iterators before destroying their backing database.
    const ref = new WeakRef(result);
    result.owner = this;
    result.derivativeRef = ref;
    this.derivatives.add(ref);
    this.derivativeGC.register(result, ref, result);
    return result;
  }

  [Symbol.iterator]() {
    return new LevelDBIterator(this.iterator());
  }

  entries() {
    return this[Symbol.iterator]();
  }

  keys() {
    return new LevelDBKeyIterator(this.iterator());
  }

  values() {
    return new LevelDBValueIterator(this.iterator());
  }
}

exports.LevelDB = LevelDB;
exports.LevelDBWriteBatch = LevelDBWriteBatch;
