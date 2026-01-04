const EmLDBModule = require("./dist/libleveldb.js");
var EmLDB = null;

function cwraps(leveldb) {
  function wrap(name, retType, argTypes) {
    var types = [];

    for (var i = 0; i < argTypes.length; i++) {
      var ch = argTypes[i];

      if (argTypes[0] == "v")
        // void.
        break;

      if (ch == "P") {
        if (argTypes[i + 1] == "k") {
          if (argTypes[i + 2] == "w") {
            // Pkw: const wchar_t *, as "string".
            i += 2;
            types.push("string");
          } else if (argTypes[i + 1] == "k" && argTypes[i + 2] == "c") {
            // Pkc: const char *, as "array" like Uint8Array.
            i += 2;
            types.push("array");
          } else {
            // Other pointer types.
            i += 2;
            types.push("number");
          }
        } else {
          // Other pointer types.
          i++;
          types.push("number");
        }
        continue;
      }
      if (/[bchijlmstxy]/.test(ch)) {
        // char, short, int, long, long long.
        types.push("number");
        continue;
      }
      if (ch == "b") {
        // bool.
        types.push("boolean");
        continue;
      }
    }

    return leveldb.cwrap(name, retType, types);
  }

  return {
    // Module.
    module: leveldb,
    readMemory: leveldb.getValue,
    readString: leveldb.UTF8ToString,
    writeMemory: leveldb.setValue,

    // Memory.
    malloc: wrap("malloc", "number", "i"),
    free: wrap("free", "void", "Pv"),

    // DB operations.
    leveldb_open: wrap("leveldb_open", "number", "PkvPkwPc"),
    leveldb_close: wrap("leveldb_close", "void", "Pv"),
    leveldb_put: wrap("leveldb_put", "void", "PvPkvPkciPkciPc"),
    leveldb_delete: wrap("leveldb_delete", "void", "PvPkvPkciPc"),
    leveldb_write: wrap("leveldb_write", "void", "PvPkvPvPc"),
    leveldb_get: wrap("leveldb_get", "number", "PvPkvPkciPiPc"),
    leveldb_create_iterator: wrap("leveldb_create_iterator", "number", "PvPkv"),
    leveldb_create_snapshot: wrap("leveldb_create_snapshot", "number", "Pv"),
    leveldb_release_snapshot: wrap("leveldb_release_snapshot", "number", "PvPkv"),
    leveldb_property_value: wrap("leveldb_property_value", "number", "PvPkw"),
    leveldb_approximate_sizes: wrap("leveldb_approximate_sizes", "void", "PviPkcPiPkcPiPy"),
    leveldb_compact_range: wrap("leveldb_compact_range", "void", "PvPkciPkci"),

    // Management operations.
    leveldb_destroy_db: wrap("leveldb_destroy_db", "void", "PvPkwPc"),
    leveldb_repair_db: wrap("leveldb_repair_db", "void", "PvPkwPc"),

    // Iterator.
    leveldb_iter_destroy: wrap("leveldb_iter_destroy", "void", "Pv"),
    leveldb_iter_valid: wrap("leveldb_iter_valid", "number", "Pv"),
    leveldb_iter_seek_to_first: wrap("leveldb_iter_seek_to_first", "void", "Pv"),
    leveldb_iter_seek_to_last: wrap("leveldb_iter_seek_to_last", "void", "Pv"),
    leveldb_iter_seek: wrap("leveldb_iter_seek", "void", "PvPkci"),
    leveldb_iter_next: wrap("leveldb_iter_next", "void", "Pv"),
    leveldb_iter_prev: wrap("leveldb_iter_prev", "void", "Pv"),
    leveldb_iter_key: wrap("leveldb_iter_key", "number", "PvPi"),
    leveldb_iter_value: wrap("leveldb_iter_value", "number", "PvPi"),
    leveldb_iter_get_error: wrap("leveldb_iter_get_error", "void", "PvPc"),

    // Write batch.
    leveldb_writebatch_create: wrap("leveldb_writebatch_create", "number", "v"),
    leveldb_writebatch_destroy: wrap("leveldb_writebatch_destroy", "void", "Pv"),
    leveldb_writebatch_clear: wrap("leveldb_writebatch_clear", "void", "Pv"),
    leveldb_writebatch_put: wrap("leveldb_writebatch_put", "void", "PvPkciPkci"),
    leveldb_writebatch_delete: wrap("leveldb_writebatch_delete", "void", "PvPkci"),
    leveldb_writebatch_iterate: wrap("leveldb_writebatch_iterate", "void", "PvPvPvPv"),

    // Options.
    leveldb_options_create: wrap("leveldb_options_create", "void", "v"),
    leveldb_options_destroy: wrap("leveldb_options_destroy", "void", "Pv"),
    leveldb_options_set_comparator: wrap("leveldb_options_set_comparator", "void", "PvPv"),
    leveldb_options_set_filter_policy: wrap("leveldb_options_set_filter_policy", "void", "PvPv"),
    leveldb_options_set_create_if_missing: wrap("leveldb_options_set_create_if_missing", "void", "Pvb"),
    leveldb_options_set_error_if_exists: wrap("leveldb_options_set_error_if_exists", "void", "Pvb"),
    leveldb_options_set_paranoid_checks: wrap("leveldb_options_set_paranoid_checks", "void", "Pvb"),
    leveldb_options_set_env: wrap("leveldb_options_set_env", "void", "PvPv"),
    leveldb_options_set_info_log: wrap("leveldb_options_set_info_log", "void", "PvPv"),
    leveldb_options_set_write_buffer_size: wrap("leveldb_options_set_write_buffer_size", "void", "Pvi"),
    leveldb_options_set_max_open_files: wrap("leveldb_options_set_max_open_files", "void", "Pvi"),
    leveldb_options_set_cache: wrap("leveldb_options_set_cache", "void", "PvPv"),
    leveldb_options_set_block_size: wrap("leveldb_options_set_block_size", "void", "Pvi"),
    leveldb_options_set_block_restart_interval: wrap("leveldb_options_set_block_restart_interval", "void", "Pvi"),
    leveldb_options_set_compression: wrap("leveldb_options_set_compression", "void", "Pvi"),

    // Filter policy.
    leveldb_filterpolicy_create: wrap("leveldb_filterpolicy_create", "number", "PvPvPvPvPv"),
    leveldb_filterpolicy_destroy: wrap("leveldb_filterpolicy_destroy", "void", "Pv"),
    leveldb_filterpolicy_create_bloom: wrap("leveldb_filterpolicy_create_bloom", "number", "i"),

    // Read options.
    leveldb_readoptions_create: wrap("leveldb_readoptions_create", "number", "v"),
    leveldb_readoptions_destroy: wrap("leveldb_readoptions_destroy", "void", "Pv"),
    leveldb_readoptions_set_verify_checksums: wrap("leveldb_readoptions_set_verify_checksums", "void", "Pvb"),
    leveldb_readoptions_set_fill_cache: wrap("leveldb_readoptions_set_fill_cache", "void", "Pvb"),
    leveldb_readoptions_set_snapshot: wrap("leveldb_readoptions_set_snapshot", "void", "PvPv"),

    // Write options.
    leveldb_writeoptions_create: wrap("leveldb_writeoptions_create", "number", "v"),
    leveldb_writeoptions_destroy: wrap("leveldb_writeoptions_destroy", "void", "Pv"),
    leveldb_writeoptions_set_sync: wrap("leveldb_writeoptions_set_sync", "void", "Pvb"),
  }
}

function validateError(pszErr) {
  var szErr;
  if (pszErr && (szErr = EmLDB.readMemory(pszErr, "i32")))
    return EmLDB.readString(szErr);
  return void 0;
}

class LevelDBOptions {
  static COMPRESSIONS = {
    NONE: 0,
    ZLIB: 2,
    ZLIB_RAW: 4
  };

  constructor(options = {}) {
    this.createIfMissing = options.createIfMissing ?? void 0;
    this.errorIfExists = options.errorIfExists ?? void 0;
    this.paranoidChecks = options.paranoidChecks ?? void 0;
    this.writeBufferSize = options.writeBufferSize ?? void 0;
    this.maxOpenFiles = options.maxOpenFiles ?? void 0;
    this.blockSize = options.blockSize ?? void 0;
    this.blockRestartInteval = options.blockRestartInteval ?? void 0;
    this.compression = options.compression ?? void 0;

    this.opt = 0;
  }

  serialize() {
    var opt = this.opt = EmLDB.leveldb_options_create();

    this.createIfMissing != null && EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);
    this.errorIfExists != null && EmLDB.leveldb_options_set_error_if_exists(opt, this.errorIfExists);
    //this.createIfMissing ?? EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);
    //this.createIfMissing ?? EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);
    //this.createIfMissing ?? EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);
    //this.createIfMissing ?? EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);
    //this.createIfMissing ?? EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);
    //this.createIfMissing ?? EmLDB.leveldb_options_set_create_if_missing(opt, this.createIfMissing);

    return this.opt;
  }

  free() {
    EmLDB.leveldb_options_destroy(this.opt);
    this.opt = 0;
  }
}

class LevelDBReadOptions {
  constructor(options = {}) {
    this.verifyChecksums = options.verifyChecksums ?? void 0;
    this.fillCache = options.fillCache ?? void 0;
    this.snapshot = options.snapshot ?? void 0;

    this.opt = 0;
  }

  serialize() {
    var opt = this.opt = EmLDB.leveldb_readoptions_create();

    this.verifyChecksums != null && EmLDB.leveldb_readoptions_set_sync(opt, this.verifyChecksums);
    this.fillCache != null && EmLDB.leveldb_readoptions_set_fill_cache(opt, this.fillCache);
    this.snapshot != null && EmLDB.leveldb_readoptions_set_snapshot(opt, this.snapshot);

    return this.opt;
  }

  free() {
    EmLDB.leveldb_readoptions_destroy(this.opt);
    this.opt = 0;
  }
}

class LevelDBWriteOptions {
  constructor(options = {}) {
    this.sync = options.sync ?? void 0;

    this.opt = 0;
  }

  serialize() {
    var opt = this.opt = EmLDB.leveldb_writeoptions_create();

    this.sync ?? EmLDB.leveldb_writeoptions_set_sync(opt, this.sync);

    return this.opt;
  }

  free() {
    EmLDB.leveldb_writeoptions_destroy(this.opt);
    this.opt = 0;
  }
}

class LevelDB {
  static async initialize() {
    var leveldb = await EmLDBModule();
    EmLDB = cwraps(leveldb);
    return true;
  }

  constructor(path, options) {
    if (!EmLDB)
      throw new Error("LevelDB constructor called before initialization.");

    this.path = path;
    this.options = options ?? {};
    this.db = 0;
  }

  validate() {
    if (!this.db)
      throw new Error("try to access DB before it's open.");
  }

  open() {
    var opt = new LevelDBOptions(this.options)
      , pszErr = EmLDB.malloc(4)
      , err;

    this.db = EmLDB.leveldb_open(opt.serialize(), this.path, pszErr);

    opt.free();

    if (err = validateError(pszErr)) {
      // We need to free the memory before throw an error.
      EmLDB.free(pszErr);
      throw new Error(err);
    }

    EmLDB.free(pszErr);
    return !!this.db;
  }

  close() {
    if (this.db)
      EmLDB.leveldb_close(this.db);
    else
      throw new Error("try to close DB before it's open.");

    this.db = 0;
    return true;
  }

  put(key, value, options) {
    this.validate();

    var wopt = new LevelDBWriteOptions(options)
      , pszErr = EmLDB.malloc(4)
      , err;

    if (typeof key === "string")
      key = (new TextEncoder("")).encode(key);
    if (typeof value === "string")
      value = (new TextEncoder("")).encode(value);

    EmLDB.leveldb_put(this.db, wopt.serialize(), key, key.length, value, value.length, pszErr)

    wopt.free();

    if (err = validateError(pszErr)) {
      // We need to free the memory before throw an error.
      EmLDB.free(pszErr);
      throw new Error(err);
    }

    EmLDB.free(pszErr);
    return true;
  }

  get(key, options) {
    this.validate();

    var ropt = new LevelDBReadOptions(options)
      , pLength = EmLDB.malloc(4)
      , pszErr = EmLDB.malloc(4)
      , pValue, length, result, err;

    if (typeof key === "string")
      key = (new TextEncoder("")).encode(key);

    pValue = EmLDB.leveldb_get(this.db, ropt.serialize(), key, key.length, pLength, pszErr);

    ropt.free();

    if (err = validateError(pszErr)) {
      // We need to free the memory before throw an error.
      EmLDB.free(pLength);
      EmLDB.free(pszErr);
      throw new Error(err);
    }

    if (!pValue) {
      EmLDB.free(pLength);
      EmLDB.free(pszErr);
      return void 0;
    }

    length = EmLDB.readMemory(pLength);
    result = Uint8Array.from(EmLDB.module.HEAPU8.subarray(pValue, pValue + length));

    EmLDB.free(pLength);
    EmLDB.free(pszErr);
    return result;
  }
}

exports.LevelDB = LevelDB;
