// Copyright (c) 2024 EmLDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.

/**
 * Options for database operations.
 */
export interface LevelDBOptions {
  /**
   * If true, the database will be created if it is missing.
   *
   * Default: true
   */
  createIfMissing?: boolean;

  /**
   * If true, an error is raised if the database already exists.
   *
   * Default: false
   */
  errorIfExists?: boolean;

  /**
   * If true, the implementation will do aggressive checking of the
   * data it is processing and will stop early if it detects any errors.
   *
   * Default: false
   */
  paranoidChecks?: boolean;

  /**
   * Compression used for newly written blocks. Existing zlib and raw-zlib
   * blocks remain readable regardless of this choice.
   *
   * Default: LevelDB.COMPRESSIONS.ZLIB_RAW
   */
  compression?: 0 | 2 | 4;

  /**
   * zlib compression level, from 0 through 9, or -1 for the zlib default.
   * Requires compression to be ZLIB or ZLIB_RAW.
   *
   * Default: -1
   */
  compressionLevel?: number;

  /**
   * Amount of data to build up in memory (backed by an unsorted log
   * on disk) before converting to a sorted on-disk file.
   *
   * Larger values increase performance, especially during bulk loads.
   * Up to two write buffers may be held in memory at the same time,
   * so you may wish to adjust this parameter to control memory usage.
   *
   * Default: 4MB (4194304 bytes)
   */
  writeBufferSize?: number;

  /**
   * Number of open files that can be used by the DB.  You may need to
   * increase this if your database has a large working set (budget
   * one open file per 2MB of working set).
   *
   * Default: 1000
   */
  maxOpenFiles?: number;

  /**
   * Approximate size of user data packed per block.  Note that the
   * block size specified here corresponds to uncompressed data.  The
   * actual size of the unit read from disk may be smaller if
   * compression is enabled.
   *
   * Default: 4KB (4096 bytes)
   */
  blockSize?: number;

  /**
   * Number of keys between restart points for delta encoding of keys.
   * This parameter can be changed dynamically.  Most clients should
   * leave this parameter alone.
   *
   * Default: 16
   */
  blockRestartInterval?: number;

  /**
   * LRU cache size for blocks. If non-zero, use the specified cache size.
   * If zero, leveldb will automatically create and use an 8MB internal cache.
   *
   * Default: 0 (auto 8MB)
   */
  cacheSize?: number;

  /**
   * Bloom filter bits per key. If non-zero, use the specified bloom filter.
   * A good value for bits_per_key is 10, which yields a filter with
   * ~1% false positive rate.
   *
   * Default: 0 (no bloom filter)
   */
  bloomFilterBits?: number;

  /**
   * Custom comparator function for key ordering.
   * The function should return:
   *   -1 if a < b
   *    0 if a == b
   *    1 if a > b
   *
   * REQUIRES: The comparator must have the same name and orders keys
   * *exactly* the same as the comparator provided to previous open calls.
   *
   * @deprecated JavaScript comparators are rejected by the threaded build
   * because Emscripten JS handles cannot safely cross worker threads.
   */
  comparator?: (a: Uint8Array, b: Uint8Array) => number;

  /**
   * Name for the custom comparator. Required if comparator is provided.
   *
   * @deprecated See comparator.
   */
  comparatorName?: string;

  /**
   * XOR encryption key in MCNE compatibility. MCNE support is enabled when
   * the key is provided.
   * 
   * Default: "" (disabled)
   */
  mcneEncryptionKey?: string | Uint8Array;
}

export interface LevelDBInitializationOptions {
  /** Fixed 128MB heap for maximum pthread performance, or growable heap up to 2GB. */
  memory?: 'performance' | 'scalable';
}

/**
 * Options for read operations.
 */
export interface LevelDBReadOptions {
  /**
   * If true, all data read from underlying storage will be
   * verified against corresponding checksums.
   *
   * Default: false
   */
  verifyChecksums?: boolean;

  /**
   * Should the data read for this iteration be cached in memory?
   * Callers may wish to set this field to false for bulk scans.
   *
   * Default: true
   */
  fillCache?: boolean;
}

/**
 * Options for write operations.
 */
export interface LevelDBWriteOptions {
  /**
   * If true, the write will be flushed from the operating system
   * buffer cache (by calling WritableFile::Sync()) before the write
   * is considered complete. If this flag is true, writes will be slower.
   *
   * If this flag is false, and the machine crashes, some recent
   * writes may be lost. Note that if it is just the process that
   * crashes (i.e., the machine does not reboot), no writes will be
   * lost even if sync==false.
   *
   * Default: false
   */
  sync?: boolean;
}

export type LevelDBBatchOperation =
  | { type: 'put'; key: Uint8Array | string; value: Uint8Array | string }
  | { type: 'del'; key: Uint8Array | string };

/**
 * Write batch for atomic batch operations.
 */
export class LevelDBWriteBatch {
  /**
   * Create a new write batch.
   */
  constructor();

  /**
   * Store the mapping "key->value" in the database.
   *
   * @param key - The key (string or Uint8Array)
   * @param value - The value (string or Uint8Array)
   * @returns this (for chaining)
   */
  put(key: Uint8Array | string, value: Uint8Array | string): this;

  /**
   * Remove the database entry (if any) for "key".
   *
   * @param key - The key (string or Uint8Array)
   * @returns this (for chaining)
   */
  del(key: Uint8Array | string): this;

  /**
   * Clear all updates buffered in this batch.
   *
   * @returns this (for chaining)
   */
  clear(): this;

  /** Release the native write-batch allocation. Safe to call more than once. */
  free(): void;
}

/**
 * Iterator for traversing database entries.
 */
export interface LevelDBIteratorBase {
  /**
   * An iterator is either positioned at a key/value pair, or not valid.
   *
   * @returns true if the iterator is valid
   */
  valid(): boolean;

  /**
   * Free the iterator resources.
   */
  free(): void;

  /**
   * Position at the first key in the source.
   * The iterator is valid() after this call if the source is not empty.
   */
  seekToFirst(): void;

  /**
   * Position at the last key in the source.
   * The iterator is valid() after this call if the source is not empty.
   */
  seekToLast(): void;

  /**
   * Position at the first key in the source that is at or past target.
   * The iterator is valid() after this call if the source contains
   * an entry that comes at or past target.
   *
   * @param key - The target key
   */
  seek(key: Uint8Array | string): void;

  /**
   * Moves to the next entry in the source.
   * After this call, valid() is true if the iterator was not positioned
   * at the last entry in source.
   *
   * REQUIRES: valid()
   */
  next(): void;

  /**
   * Moves to the previous entry in the source.
   * After this call, valid() is true if the iterator was not positioned
   * at the first entry in source.
   *
   * REQUIRES: valid()
   */
  prev(): void;

  /**
   * Return a copy of the key for the current entry.
   *
   * REQUIRES: valid()
   * @returns The key as Uint8Array, or undefined if not valid
   */
  key(): Uint8Array | undefined;

  /**
   * Return a copy of the value for the current entry.
   *
   * REQUIRES: valid()
   * @returns The value as Uint8Array, or undefined if not valid
   */
  value(): Uint8Array | undefined;

  /**
   * If an error has occurred, throw it. Else do nothing.
   *
   * @throws Error if iterator has an error
   */
  getError(): void;
}

/**
 * JavaScript iterator for key-value pairs.
 */
export interface LevelDBIterator extends Iterator<[Uint8Array, Uint8Array]> {
  next(): IteratorResult<[Uint8Array, Uint8Array]>;
  return(): IteratorResult<[Uint8Array, Uint8Array]>;
  [Symbol.iterator](): this;
}

/**
 * JavaScript iterator for keys only.
 */
export interface LevelDBKeyIterator extends Iterator<Uint8Array> {
  next(): IteratorResult<Uint8Array>;
  return(): IteratorResult<Uint8Array>;
  [Symbol.iterator](): this;
}

/**
 * JavaScript iterator for values only.
 */
export interface LevelDBValueIterator extends Iterator<Uint8Array> {
  next(): IteratorResult<Uint8Array>;
  return(): IteratorResult<Uint8Array>;
  [Symbol.iterator](): this;
}

/**
 * Main LevelDB database class.
 */
export class LevelDB {
  /**
   * Compression type constants.
   */
  static readonly COMPRESSIONS: {
    readonly NONE: 0;
    /** File-format ID only; the bundled build rejects SNAPPY for writes. */
    readonly SNAPPY: 1;
    readonly ZLIB: 2;
    readonly ZLIB_RAW: 4;
  };

  /**
   * Initialize the WebAssembly module.
   * Must be called before any database operations.
   *
   * @returns Promise that resolves to true when initialized
   */
  static initialize(options?: LevelDBInitializationOptions): Promise<true>;

  /** The selected profile after initialization, otherwise null. */
  static readonly memoryProfile: 'performance' | 'scalable' | null;

  /**
   * Destroy the contents of the specified database.
   * Be very careful using this method.
   *
   * @param path - Path to the database
   * @param options - Database options
   * @throws Error if destroy fails
   */
  static destroy(path: string, options?: LevelDBOptions): void;

  /**
   * If a DB cannot be opened, you may attempt to call this method to
   * resurrect as much of the contents of the database as possible.
   * Some data may be lost, so be careful when calling this function
   * on a database that contains important information.
   *
   * @param path - Path to the database
   * @param options - Database options
   * @throws Error if repair fails
   */
  static repair(path: string, options?: LevelDBOptions): void;

  /**
   * If a database uses NetEase's 8-byte XOR encryption, you may need to
   * call this function to infer its key. The function returns an 8-byte
   * Uint8Array, or undefined if the inference fails.
   *
   * @param path - Path to the database
   * @param options - Database options
   */
  static infer(path: string, options?: LevelDBOptions): Uint8Array | undefined;

  /**
   * This function performs decryption in place on the encrypted database,
   * or performs decryption on an unencrypted database. This function modifies
   * files, so call it with caution.
   *
   * @param path - Path to the database
   * @param options - Database options
   * @param key - Encryption key
   */
  static encrypt(path: string, options?: LevelDBOptions, key: Uint8Array): void;

  /**
   * Create a new LevelDB instance.
   *
   * @param path - Path to the database
   * @param options - Database options
   */
  constructor(path: string, options?: LevelDBOptions);

  /**
   * Open the database.
   *
   * @returns true if successful
   * @throws Error if open fails
   */
  open(): boolean;

  /**
   * Close the database.
   *
   * @returns true if successful
   * @throws Error if close fails
   */
  close(): boolean;

  /**
   * Set the database entry for "key" to "value".
   *
   * @param key - The key (string or Uint8Array)
   * @param value - The value (string or Uint8Array)
   * @param options - Write options
   * @returns true if successful
   * @throws Error if put fails
   */
  put(key: Uint8Array | string, value: Uint8Array | string, options?: LevelDBWriteOptions): boolean;

  /**
   * Remove the database entry (if any) for "key".
   * It is not an error if "key" did not exist in the database.
   *
   * @param key - The key (string or Uint8Array)
   * @param options - Write options
   * @returns true if successful
   * @throws Error if delete fails
   */
  delete(key: Uint8Array | string, options?: LevelDBWriteOptions): boolean;

  /**
   * Apply the specified updates to the database atomically.
   *
   * @param batch - The write batch
   * @param options - Write options
   * @returns true if successful
   * @throws Error if write fails
   */
  write(batch: LevelDBWriteBatch, options?: LevelDBWriteOptions): boolean;

  /**
   * Create a write batch with the specified operations.
   *
   * @param operations - Array of operations
   * @returns A new write batch
   */
  batch(operations?: LevelDBBatchOperation[]): LevelDBWriteBatch;

  /**
   * If the database contains an entry for "key", return the value.
   *
   * @param key - The key (string or Uint8Array)
   * @param options - Read options
   * @returns The value as Uint8Array, or undefined if not found
   * @throws Error if get fails
   */
  get(key: Uint8Array | string, options?: LevelDBReadOptions): Uint8Array | undefined;

  /**
   * Compact the underlying storage for the key range [startKey, limitKey].
   * In particular, deleted and overwritten versions are discarded,
   * and the data is rearranged to reduce the cost of operations
   * needed to access the data.
   *
   * Note: This operation is currently experimental and may be unstable.
   *
   * @param startKey - Start of the key range (inclusive)
   * @param limitKey - End of the key range (exclusive)
   */
  compact(startKey?: Uint8Array | string | null, limitKey?: Uint8Array | string | null): void;

  /**
   * Get database property value.
   *
   * Valid property names include:
   *   "leveldb.num-files-at-level<N>" - return the number of files at level <N>
   *   "leveldb.stats" - returns a multi-line string with general statistics
   *   "leveldb.sstables" - returns a multi-line string with SSTable info
   *   "leveldb.approximate-memory-usage" - approximate memory usage
   *
   * @param property - Property name
   * @returns Property value as string, or empty string if not found
   */
  getProperty(property: string): string;

  /**
   * Return an iterator over the contents of the database.
   * The result of iterator() is initially invalid (caller must call one of
   * the seek methods on the iterator before using it).
   *
   * @param options - Read options
   * @returns A new iterator
   */
  iterator(options?: LevelDBReadOptions): LevelDBIteratorBase;

  /**
   * Get an iterator for key-value pairs.
   * Supports for...of syntax.
   *
   * @returns An iterable iterator
   */
  [Symbol.iterator](): LevelDBIterator;

  /**
   * Get an iterator for key-value pairs.
   *
   * @returns An iterable iterator
   */
  entries(): LevelDBIterator;

  /**
   * Get an iterator for keys only.
   *
   * @returns An iterable iterator
   */
  keys(): LevelDBKeyIterator;

  /**
   * Get an iterator for values only.
   *
   * @returns An iterable iterator
   */
  values(): LevelDBValueIterator;
}

