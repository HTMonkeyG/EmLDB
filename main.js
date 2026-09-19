const EmLDBModule = require("./dist/libleveldb.js");
var EmLDB = null;

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

// ----------------------------------------------------------------------------
// [SECTION] OPTIONS
// ----------------------------------------------------------------------------

class LevelDBOptions {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(options = {}) {
    this.opt = new EmLDB.Options();

    LevelDBOptions.GC.register(this, this.opt, this);

    // Apply options
    if (options.createIfMissing != null)
      this.opt.setCreateIfMissing(options.createIfMissing);
    if (options.errorIfExists != null)
      this.opt.setErrorIfExists(options.errorIfExists);
    if (options.paranoidChecks != null)
      this.opt.setParanoidChecks(options.paranoidChecks);
    if (options.writeBufferSize != null)
      this.opt.setWriteBufferSize(options.writeBufferSize);
    if (options.maxOpenFiles != null)
      this.opt.setMaxOpenFiles(options.maxOpenFiles);
    if (options.blockSize != null)
      this.opt.setBlockSize(options.blockSize);
    if (options.blockRestartInterval != null)
      this.opt.setBlockRestartInterval(options.blockRestartInterval);
    if (options.cacheSize != null)
      this.opt.setCache(options.cacheSize);
    if (options.bloomFilterBits != null)
      this.opt.setBloomFilter(options.bloomFilterBits);

    // Custom comparator support
    if (options.comparator != null && typeof options.comparator === "function") {
      this.opt.setComparator(options.comparator, options.comparatorName || "custom");
    }
  }

  get() {
    return this.opt;
  }

  free() {
    if (!this.opt)
      return;

    LevelDBOptions.GC.unregister(this);
    this.opt.delete();
    this.opt = null;
  }
}

class LevelDBReadOptions {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(options = {}) {
    this.opt = new EmLDB.ReadOptions();

    LevelDBReadOptions.GC.register(this, this.opt, this);

    if (options.verifyChecksums != null)
      this.opt.setVerifyChecksums(options.verifyChecksums);
    if (options.fillCache != null)
      this.opt.setFillCache(options.fillCache);
  }

  get() {
    return this.opt;
  }

  free() {
    if (!this.opt)
      return;

    LevelDBReadOptions.GC.unregister(this);
    this.opt.delete();
    this.opt = null;
  }
}

class LevelDBWriteOptions {
  static GC = createGCListener(function (value) {
    value.delete();
  });

  constructor(options = {}) {
    this.opt = new EmLDB.WriteOptions();

    LevelDBWriteOptions.GC.register(this, this.opt, this);

    if (options.sync != null)
      this.opt.setSync(options.sync);
  }

  get() {
    return this.opt;
  }

  free() {
    if (!this.opt)
      return;

    LevelDBWriteOptions.GC.unregister(this);
    this.opt.delete();
    this.opt = null;
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
    this.batch = new EmLDB.WriteBatch();
    LevelDBWriteBatch.GC.register(this, this.batch, this);
  }

  put(key, value) {
    key = toUint8Array(key);
    value = toUint8Array(value);
    this.batch.put(key, value);
    return this;
  }

  del(key) {
    key = toUint8Array(key);
    this.batch.del(key);
    return this;
  }

  clear() {
    this.batch.clear();
    return this;
  }

  get() {
    return this.batch;
  }

  free() {
    if (!this.batch)
      return;

    LevelDBWriteBatch.GC.unregister(this);
    this.batch.delete();
    this.batch = null;
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

    this.iter.delete();
    LevelDBIteratorBase.GC.unregister(this);
    this.iter = null;
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
    this.base.seekToFirst();
  }

  next() {
    if (!this.base.valid()) {
      return {
        done: true
      };
    }

    var key = this.base.key();
    var value = this.base.value();
    var valid;

    this.base.next();
    valid = this.base.valid();

    if (!valid)
      this.base.free();

    return {
      value: [key, value],
      done: false
    };
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
  static COMPRESSIONS = {
    NONE: 0,
    SNAPPY: 1,
    ZLIB: 2,
    ZLIB_RAW: 4
  };

  static async initialize() {
    var module = await EmLDBModule();
    EmLDB = module;
    return true;
  }

  static destroy(path, options) {
    if (!EmLDB)
      throw new Error("LevelDB not initialized.");

    var opt = new LevelDBOptions(options);
    try {
      EmLDB.DB.destroy(path, opt.get());
    } finally {
      opt.free();
    }
  }

  static repair(path, options) {
    if (!EmLDB)
      throw new Error("LevelDB not initialized.");

    var opt = new LevelDBOptions(options);
    try {
      EmLDB.DB.repair(path, opt.get());
    } finally {
      opt.free();
    }
  }

  constructor(path, options) {
    if (!EmLDB)
      throw new Error("LevelDB not initialized.");

    this.path = path;
    this.options = options || {};
    this.db = null;
    this.derivatives = [];
  }

  validate() {
    if (!this.db)
      throw new Error("try to access DB before it's open.");
  }

  open() {
    var opt = new LevelDBOptions(this.options);

    try {
      this.db = new EmLDB.DB();
      this.db.open(this.path, opt.get());
      return true;
    } finally {
      opt.free();
    }
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
        obj.free();
      } catch (e) {
        // Iterator might already be freed, ignore
      }
    }

    try {
      this.db.close();
    } catch (e) {
      console.error('Error closing database:', e);
    }

    this.db.delete();
    this.derivatives = [];
    this.db = null;
    return true;
  }

  put(key, value, options) {
    this.validate();

    var wopt = new LevelDBWriteOptions(options);
    key = toUint8Array(key);
    value = toUint8Array(value);

    try {
      this.db.put(key, value, wopt.get());
      return true;
    } finally {
      wopt.free();
    }
  }

  delete(key, options) {
    this.validate();

    var wopt = new LevelDBWriteOptions(options);
    key = toUint8Array(key);

    try {
      this.db.del(key, wopt.get());
      return true;
    } finally {
      wopt.free();
    }
  }

  write(batch, options) {
    this.validate();

    var wopt = new LevelDBWriteOptions(options);

    try {
      this.db.write(batch.get(), wopt.get());
      return true;
    } finally {
      wopt.free();
    }
  }

  batch(operations) {
    var batch = new LevelDBWriteBatch();

    if (Array.isArray(operations)) {
      for (var op of operations) {
        if (op.type === "put")
          batch.put(op.key, op.value);
        else if (op.type === "del")
          batch.del(op.key);
      }
    }

    return batch;
  }

  get(key, options) {
    this.validate();

    var ropt = new LevelDBReadOptions(options);
    key = toUint8Array(key);

    try {
      return this.db.get(key, ropt.get());
    } finally {
      ropt.free();
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
    return this.db.getProperty(property);
  }

  iterator(options) {
    this.validate();

    var ropt = new LevelDBReadOptions(options);
    var iter;

    try {
      iter = this.db.createIterator(ropt.get());
    } finally {
      ropt.free();
    }

    var result = new LevelDBIteratorBase(iter);

    // Add a WeakRef to record all derived objects
    this.derivatives.push(new WeakRef(result));
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
