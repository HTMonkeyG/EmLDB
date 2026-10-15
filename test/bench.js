// bench.js
//
// A JavaScript port of leveldb's own benchmark, deps/leveldb-mcpe/db/db_bench.cc,
// to the EmLDB API. Benchmark set, flags, timing method, histogram and output
// format all follow db_bench.cc so results can be read the same way.
//
// Deviations, forced by the wasm / synchronous-JS environment, are listed by
// --help and printed by PrintHeader():
//
//   * single threaded  - EmLDB's API is synchronous, there is no --threads.
//   * compact          - timed the same way db_bench times it: DBImpl::
//                        CompactRange() blocks until its manual compactions are
//                        done (db_impl.cc TEST_CompactRange waits on bg_cv_), so
//                        the measured time is the real compaction time.
//   * default --num    - db_bench defaults to 1000000; wasm is roughly an order
//                        of magnitude slower, so the default here is 100000.
//                        Pass --num=1000000 for the original workload.
//   * no --max_file_size - EmLDB exposes no option for it.
//   * CPU-only benchmarks of db_bench (crc32c, snappycomp, snappyuncomp,
//     acquireload, heapprofile) are omitted: they measure leveldb's C++
//     helpers, not this binding.
//
// Usage: node bench.js --benchmarks=fillseq,readrandom --num=100000

'use strict';

const path = require('path');
const { LevelDB } = require('../main.js');

// EmLDB raises JavaScript Errors, so e.message carries the real diagnostic.
// The fallback still matters: never swallow a failure, because a swallowed
// error turns a broken database into a fast-looking benchmark result.
function errorText(e) {
  if (e instanceof Error) return e.message;
  return String(e);
}

function die(what, e) {
  process.stderr.write(`\n${what}: ${errorText(e)}\n`);
  process.exit(1);
}

// Mirrors leveldb::Env::NowMicros(). Keep the fraction: hrtime is nanoseconds.
const nowMicros = () => Number(process.hrtime.bigint()) / 1000;

// ----------------------------------------------------------------------------
// Random (util/random.h)
// ----------------------------------------------------------------------------

class Random {
  constructor(seed = 301) {
    this.seed = seed >>> 0;
  }

  // Exact port of Random::Next(): A = 16807, M = 2^31-1, with the 31-bit fold.
  next() {
    const M = 2147483647;
    const product = this.seed * 16807; // <= 2^31 * 16807, well inside 2^53
    let s = Math.floor(product / 2147483648) + (product % 2147483648);
    if (s > M) s -= M;
    this.seed = s >>> 0;
    return this.seed;
  }

  uniform(n) {
    return this.next() % n;
  }

  oneIn(n) {
    return this.uniform(n) === 0;
  }

  // util/random.h: pick a base uniformly from [0, maxLog] then return that many
  // random bits, biasing the result towards small numbers.
  skewed(maxLog) {
    return this.uniform(2 ** this.uniform(maxLog + 1));
  }
}

// ----------------------------------------------------------------------------
// Value generation (util/testutil.cc + db_bench.cc)
// ----------------------------------------------------------------------------

// Exact port of test::CompressibleString(): `fraction` of the value is random
// bytes, repeated to fill `len` bytes. fraction 0.5 therefore compresses to
// roughly half its size.
function compressibleString(rnd, fraction, len) {
  let raw = Math.floor(len * fraction);
  if (raw < 1) raw = 1;

  const source = Buffer.alloc(raw);
  for (let i = 0; i < raw; i++) {
    source[i] = 32 + rnd.uniform(95); // ' ' .. '~'
  }

  const out = Buffer.alloc(len);
  for (let i = 0; i < len; i++) {
    out[i] = source[i % raw];
  }
  return out;
}

// Exact port of db_bench's RandomGenerator: 1MB of pieces larger than the zlib
// window, replayed over and over.
class RandomGenerator {
  constructor(compressionRatio) {
    const rnd = new Random(301);
    const chunks = [];
    let size = 0;
    while (size < 1048576) {
      const piece = compressibleString(rnd, compressionRatio, 100);
      chunks.push(piece);
      size += piece.length;
    }
    this.data = Buffer.concat(chunks);
    this.pos = 0;
  }

  generate(len) {
    if (this.pos + len > this.data.length) {
      this.pos = 0;
    }
    const start = this.pos;
    this.pos += len;
    return this.data.subarray(start, start + len);
  }
}

// ----------------------------------------------------------------------------
// Histogram (util/histogram.cc)
// ----------------------------------------------------------------------------

const NUM_BUCKETS = 154;

// Same bucket series as Histogram::kBucketLimit: six decades of
// {1..9} followed by {10,12,...,90}. The tail is cut at kNumBuckets, so
// anything larger lands in the last bucket.
const BUCKET_LIMITS = (() => {
  const small = [1, 2, 3, 4, 5, 6, 7, 8, 9];
  const large = [10, 12, 14, 16, 18, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90];
  const limits = [];
  let base = 1;
  while (limits.length < NUM_BUCKETS) {
    for (const m of small) limits.push(base * m);
    for (const m of large) limits.push(base * m);
    base *= 100;
  }
  limits.length = NUM_BUCKETS;
  return limits;
})();

class Histogram {
  constructor() {
    this.clear();
  }

  clear() {
    this.buckets = new Float64Array(NUM_BUCKETS);
    this.min = 0;
    this.max = 0;
    this.count = 0;
    this.sum = 0;
    this.sumSquares = 0;
  }

  findBucket(value) {
    let left = 0;
    let right = NUM_BUCKETS - 1;
    while (left < right) {
      const middle = (left + right) >> 1;
      if (BUCKET_LIMITS[middle] <= value) left = middle + 1;
      else right = middle;
    }
    return left;
  }

  add(value) {
    const b = this.findBucket(value);
    this.buckets[b] += 1;

    if (this.count === 0) {
      this.min = value;
      this.max = value;
    } else {
      if (value < this.min) this.min = value;
      if (value > this.max) this.max = value;
    }
    this.count++;
    this.sum += value;
    this.sumSquares += value * value;
  }

  average() {
    return this.count ? this.sum / this.count : 0;
  }

  standardDeviation() {
    if (this.count < 1) return 0;
    const variance = (this.sumSquares - (this.sum * this.sum) / this.count) / (this.count - 1);
    return Math.sqrt(Math.max(variance, 0));
  }

  // Linear interpolation inside the bucket that crosses the threshold.
  percentile(p) {
    if (this.count === 0) return 0;
    const threshold = this.count * (p / 100);
    let sum = 0;
    for (let b = 0; b < NUM_BUCKETS; b++) {
      sum += this.buckets[b];
      if (sum >= threshold) {
        const leftPoint = b === 0 ? 0 : BUCKET_LIMITS[b - 1];
        const rightPoint = BUCKET_LIMITS[b];
        const leftSum = sum - this.buckets[b];
        const pos = (threshold - leftSum) / (sum - leftSum);
        let r = leftPoint + (rightPoint - leftPoint) * pos;
        if (r < this.min) r = this.min;
        if (r > this.max) r = this.max;
        return r;
      }
    }
    return this.max;
  }

  median() {
    return this.percentile(50);
  }

  toString() {
    const lines = [];
    lines.push(
      `Count: ${this.count.toFixed(0)}  Average: ${this.average().toFixed(4)}  ` +
        `StdDev: ${this.standardDeviation().toFixed(2)}`
    );
    lines.push(
      `Min: ${(this.count === 0 ? 0 : this.min).toFixed(4)}  ` +
        `Median: ${this.median().toFixed(4)}  Max: ${this.max.toFixed(4)}`
    );
    lines.push('-'.repeat(54));

    const mult = 100 / this.count;
    let cumulative = 0;
    for (let b = 0; b < NUM_BUCKETS; b++) {
      if (this.buckets[b] <= 0) continue;
      cumulative += this.buckets[b];
      const left = b === 0 ? 0 : BUCKET_LIMITS[b - 1];
      lines.push(
        `[ ${left.toFixed(0).padStart(7)}, ${BUCKET_LIMITS[b].toFixed(0).padStart(7)} ) ` +
          `${this.buckets[b].toFixed(0).padStart(7)} ` +
          `${(mult * this.buckets[b]).toFixed(3).padStart(7)}% ` +
          `${(mult * cumulative).toFixed(3).padStart(7)}%`
      );
    }
    return lines.join('\n');
  }
}

// ----------------------------------------------------------------------------
// Stats (db_bench.cc)
// ----------------------------------------------------------------------------

class Stats {
  constructor(histogramEnabled) {
    this.histogramEnabled = histogramEnabled;
    this.hist = new Histogram();
    this.start();
  }

  start() {
    this.nextReport = 100;
    this.hist.clear();
    this.done = 0;
    this.bytes = 0;
    this.seconds = 0;
    this.startTime = nowMicros();
    this.finishTime = this.startTime;
    this.lastOpFinish = this.startTime;
    this.message = '';
  }

  stop() {
    this.finishTime = nowMicros();
    this.seconds = (this.finishTime - this.startTime) * 1e-6;
  }

  addMessage(msg) {
    if (!msg) return;
    if (this.message) this.message += ' ';
    this.message += msg;
  }

  finishedSingleOp() {
    if (this.histogramEnabled) {
      const now = nowMicros();
      const micros = now - this.lastOpFinish;
      this.hist.add(micros);
      if (micros > 20000) {
        process.stderr.write(`long op: ${micros.toFixed(1)} micros${' '.repeat(30)}\r`);
      }
      this.lastOpFinish = now;
    }

    this.done++;
    if (this.done >= this.nextReport) {
      if (this.nextReport < 1000) this.nextReport += 100;
      else if (this.nextReport < 5000) this.nextReport += 500;
      else if (this.nextReport < 10000) this.nextReport += 1000;
      else if (this.nextReport < 50000) this.nextReport += 5000;
      else if (this.nextReport < 100000) this.nextReport += 10000;
      else if (this.nextReport < 500000) this.nextReport += 50000;
      else this.nextReport += 100000;
      process.stderr.write(`... finished ${this.done} ops${' '.repeat(30)}\r`);
    }
  }

  addBytes(n) {
    this.bytes += n;
  }

  report(name) {
    // Pretend at least one op was done, for benchmarks that never call
    // finishedSingleOp().
    if (this.done < 1) this.done = 1;

    let extra = '';
    if (this.bytes > 0) {
      // Rate over actual elapsed time.
      const elapsed = (this.finishTime - this.startTime) * 1e-6;
      extra = `${((this.bytes / 1048576.0) / elapsed).toFixed(1).padStart(6)} MB/s`;
    }
    if (this.message) extra += (extra ? ' ' : '') + this.message;

    process.stderr.write('\r' + ' '.repeat(70) + '\r'); // clear the progress line
    process.stdout.write(
      `${name.padEnd(12)} : ${((this.seconds * 1e6) / this.done).toFixed(3).padStart(11)} micros/op;` +
        `${extra ? ' ' : ''}${extra}\n`
    );
    if (this.histogramEnabled) {
      process.stdout.write(`Microseconds per op:\n${this.hist.toString()}\n`);
    }
  }
}

// ----------------------------------------------------------------------------
// Benchmark (db_bench.cc)
// ----------------------------------------------------------------------------

class Benchmark {
  constructor(flags) {
    this.flags = flags;
    this.db = null;
    this.num = flags.num;
    this.reads = flags.reads < 0 ? flags.num : flags.reads;
    this.valueSize = flags.value_size;
    this.entriesPerBatch = 1;
    this.writeOptions = undefined;
    // leveldb-mcpe's ReadOptions() defaults to verify_checksums=false and
    // fill_cache=true (include/leveldb/options.h); db_bench relies on those
    // defaults, so state them explicitly here.
    this.readOptions = { verifyChecksums: false, fillCache: true };
    this.stats = null;
  }

  printHeader() {
    const f = this.flags;
    const keySize = f.key_size;
    const out = [];
    out.push('-'.repeat(48));
    out.push(`DB         : ${f.db}`);
    out.push(`Keys:      ${keySize} bytes each`);
    out.push(
      `Values:    ${f.value_size} bytes each ` +
        `(${Math.floor(f.value_size * f.compression_ratio)} bytes after compression)`
    );
    out.push(`Entries:   ${f.num}`);
    out.push(`Memory:    ${f.memory_profile}`);
    out.push(
      `WriteBuffer: ${f.file_write_buffer === 0 ? '0 (flush every write)' : `${f.file_write_buffer} bytes`}`
    );
    out.push(`Compression: ${f.compression}${f.compression_level === -1 ? '' : ` (level ${f.compression_level})`}`);
    out.push(
      `Batch:     ${f.batch_size} entries (fillbatch defaults to 1000 when set to 1)`
    );
    out.push(
      `RawSize:   ${(((keySize + f.value_size) * f.num) / 1048576).toFixed(1)} MB (estimated)`
    );
    out.push(
      `FileSize:  ${(((keySize + f.value_size * f.compression_ratio) * f.num) / 1048576).toFixed(1)} MB (estimated)`
    );
    out.push('-'.repeat(48));
    out.push('EmLDB port notes: synchronous API;');
    out.push("default --num is reduced to 100000 from db_bench's 1000000.");
    out.push('-'.repeat(48));
    process.stdout.write(out.join('\n') + '\n');
  }

  open() {
    if (this.db) return;
    const f = this.flags;
    const options = { createIfMissing: !f.use_existing_db };

    // Only pass what EmLDB actually exposes; db_bench's --max_file_size has no
    // equivalent and is reported as unsupported at parse time.
    if (f.write_buffer_size) options.writeBufferSize = f.write_buffer_size;
    if (f.block_size) options.blockSize = f.block_size;
    if (f.cache_size >= 0) options.cacheSize = f.cache_size;
    if (f.bloom_bits >= 0) options.bloomFilterBits = f.bloom_bits;
    if (f.open_files) options.maxOpenFiles = f.open_files;
    options.compression = f.compression_id;
    if (f.compression_level !== -1) options.compressionLevel = f.compression_level;

    this.db = new LevelDB(f.db, options);
    try {
      this.db.open();
    } catch (e) {
      die('open error', e);
    }
  }

  close() {
    if (!this.db) return;
    try {
      this.db.close();
    } catch (e) {
      die('close error', e);
    }
    this.db = null;
  }

  // ---- write ---------------------------------------------------------------

  doWrite(thread, seq) {
    const num = this.num;
    const valueSize = this.valueSize;
    const perBatch = this.entriesPerBatch;

    if (num !== this.flags.num) {
      thread.stats.addMessage(`(${num} ops)`);
    }

    const gen = new RandomGenerator(this.flags.compression_ratio);
    let bytes = 0;

    for (let i = 0; i < num; i += perBatch) {
      const ops = [];
      for (let j = 0; j < perBatch && i + j < num; j++) {
        const k = seq ? i + j : thread.rand.next() % this.flags.num;
        const key = String(k).padStart(16, '0');
        ops.push({ type: 'put', key, value: gen.generate(valueSize) });
        bytes += valueSize + key.length;
        thread.stats.finishedSingleOp();
      }

      try {
        if (perBatch === 1) {
          this.db.put(ops[0].key, ops[0].value, this.writeOptions);
        } else {
          const batch = this.db.batch(ops);
          try {
            this.db.write(batch, this.writeOptions);
          } finally {
            batch.free();
          }
        }
      } catch (e) {
        die('put error', e);
      }
    }
    thread.stats.addBytes(bytes);
  }

  writeSeq(thread) {
    this.doWrite(thread, true);
  }

  writeRandom(thread) {
    this.doWrite(thread, false);
  }

  // ---- read ----------------------------------------------------------------

  readSequential(thread) {
    const iter = this.db.iterator(this.readOptions);
    let i = 0;
    let bytes = 0;
    try {
      for (iter.seekToFirst(); i < this.reads && iter.valid(); iter.next()) {
        bytes += iter.key().length + iter.value().length;
        thread.stats.finishedSingleOp();
        i++;
      }
      iter.getError(); // throws if the iterator hit a real error
    } catch (e) {
      die('iterate error', e);
    } finally {
      iter.free();
    }
    thread.stats.addBytes(bytes);
  }

  readReverse(thread) {
    const iter = this.db.iterator(this.readOptions);
    let i = 0;
    let bytes = 0;
    try {
      for (iter.seekToLast(); i < this.reads && iter.valid(); iter.prev()) {
        bytes += iter.key().length + iter.value().length;
        thread.stats.finishedSingleOp();
        i++;
      }
      iter.getError();
    } catch (e) {
      die('iterate error', e);
    } finally {
      iter.free();
    }
    thread.stats.addBytes(bytes);
  }

  // db_bench counts a read as found when Get() returns ok. EmLDB returns
  // undefined for NotFound and throws for a real failure, so only undefined
  // counts as missing.
  getKey(key) {
    let value;
    try {
      value = this.db.get(key, this.readOptions);
    } catch (e) {
      die('get error', e);
    }
    return value;
  }

  readRandom(thread) {
    let found = 0;
    for (let i = 0; i < this.reads; i++) {
      const key = String(thread.rand.next() % this.flags.num).padStart(16, '0');
      if (this.getKey(key) !== undefined) found++;
      thread.stats.finishedSingleOp();
    }
    thread.stats.addMessage(`(${found} of ${this.num} found)`);
  }

  readMissing(thread) {
    for (let i = 0; i < this.reads; i++) {
      const key = String(thread.rand.next() % this.flags.num).padStart(16, '0') + '.';
      this.getKey(key); // always missing by construction
      thread.stats.finishedSingleOp();
    }
  }

  readHot(thread) {
    const range = Math.floor((this.flags.num + 99) / 100);
    for (let i = 0; i < this.reads; i++) {
      const key = String(thread.rand.next() % range).padStart(16, '0');
      this.getKey(key);
      thread.stats.finishedSingleOp();
    }
  }

  seekRandom(thread) {
    let found = 0;
    for (let i = 0; i < this.reads; i++) {
      const key = String(thread.rand.next() % this.flags.num).padStart(16, '0');
      const iter = this.db.iterator(this.readOptions);
      try {
        iter.seek(key);
        if (iter.valid() && Buffer.from(iter.key()).toString('latin1') === key) found++;
        iter.getError();
      } catch (e) {
        die('seek error', e);
      } finally {
        iter.free();
      }
      thread.stats.finishedSingleOp();
    }
    thread.stats.addMessage(`(${found} of ${this.num} found)`);
  }

  // ---- delete --------------------------------------------------------------

  doDelete(thread, seq) {
    const perBatch = this.entriesPerBatch;
    for (let i = 0; i < this.num; i += perBatch) {
      const ops = [];
      for (let j = 0; j < perBatch && i + j < this.num; j++) {
        const k = seq ? i + j : thread.rand.next() % this.flags.num;
        ops.push({ type: 'del', key: String(k).padStart(16, '0') });
        thread.stats.finishedSingleOp();
      }
      try {
        if (perBatch === 1) this.db.delete(ops[0].key, this.writeOptions);
        else {
          const batch = this.db.batch(ops);
          try {
            this.db.write(batch, this.writeOptions);
          } finally {
            batch.free();
          }
        }
      } catch (e) {
        die('del error', e);
      }
    }
  }

  deleteSeq(thread) {
    this.doDelete(thread, true);
  }

  deleteRandom(thread) {
    this.doDelete(thread, false);
  }

  // ---- misc ----------------------------------------------------------------

  openBench(thread) {
    for (let i = 0; i < this.num; i++) {
      this.close();
      this.open();
      thread.stats.finishedSingleOp();
    }
  }

  // db_bench's Compact() is just CompactRange(NULL, NULL) with no
  // FinishedSingleOp(), so Report() prints the total wall time as "micros/op".
  // That is kept, with one added message to make the number readable.
  compact(thread) {
    try {
      this.db.compact(); // no range = whole DB, like CompactRange(NULL, NULL)
    } catch (e) {
      die('compact error', e);
    }
    thread.stats.addMessage('(one op = the whole compaction)');
  }

  printStats(property) {
    let stats;
    try {
      stats = this.db.getProperty(property);
    } catch (e) {
      stats = '';
    }
    process.stdout.write(`\n${stats || '(failed)'}\n`);
  }

  // ---- driver --------------------------------------------------------------

  benchmark(name) {
    // Everything db_bench resets before each benchmark.
    this.num = this.flags.num;
    this.reads = this.flags.reads < 0 ? this.flags.num : this.flags.reads;
    this.valueSize = this.flags.value_size;
    this.entriesPerBatch = this.flags.batch_size;
    this.writeOptions = undefined;

    let method = null;
    let freshDb = false;

    switch (name) {
      case 'open':
        this.num = Math.max(1, Math.floor(this.num / 10000));
        method = () => this.openBench(thread);
        break;
      case 'fillseq':
        freshDb = true;
        method = () => this.writeSeq(thread);
        break;
      case 'fillbatch':
        freshDb = true;
        this.entriesPerBatch = this.flags.batch_size > 1 ? this.flags.batch_size : 1000;
        method = () => this.writeSeq(thread);
        break;
      case 'fillrandom':
        freshDb = true;
        method = () => this.writeRandom(thread);
        break;
      case 'overwrite':
        method = () => this.writeRandom(thread);
        break;
      case 'fillsync':
        freshDb = true;
        this.num = Math.floor(this.num / 1000);
        this.writeOptions = { sync: true };
        method = () => this.writeRandom(thread);
        break;
      case 'fill100K':
        freshDb = true;
        this.num = Math.floor(this.num / 1000);
        this.valueSize = 100 * 1000;
        method = () => this.writeRandom(thread);
        break;
      case 'readseq':
        method = () => this.readSequential(thread);
        break;
      case 'readreverse':
        method = () => this.readReverse(thread);
        break;
      case 'readrandom':
        method = () => this.readRandom(thread);
        break;
      case 'readrandomsmall':
        this.reads = Math.floor(this.reads / 1000);
        method = () => this.readRandom(thread);
        break;
      case 'readmissing':
        method = () => this.readMissing(thread);
        break;
      case 'readhot':
        method = () => this.readHot(thread);
        break;
      case 'seekrandom':
        method = () => this.seekRandom(thread);
        break;
      case 'deleteseq':
        method = () => this.deleteSeq(thread);
        break;
      case 'deleterandom':
        method = () => this.deleteRandom(thread);
        break;
      case 'compact':
        method = () => this.compact(thread);
        break;
      case 'stats':
        method = () => this.printStats('leveldb.stats');
        break;
      case 'sstables':
        method = () => this.printStats('leveldb.sstables');
        break;
      default:
        process.stderr.write(`unknown benchmark '${name}'\n`);
        return;
    }

    if (freshDb) {
      if (this.flags.use_existing_db) {
        process.stdout.write(
          `${name.padEnd(12)} : skipped (--use_existing_db is true)\n`
        );
        return;
      }
      this.close();
      try {
        LevelDB.destroy(this.flags.db);
      } catch (e) {
        // A missing database is fine, anything else is not.
      }
      this.open();
    }

    const thread = { stats: new Stats(this.flags.histogram), rand: new Random(301) };
    thread.stats.start();
    method();
    thread.stats.stop();
    thread.stats.report(name);
  }

  run() {
    this.printHeader();
    this.open();

    for (const raw of this.flags.benchmarks.split(',')) {
      const name = raw.trim();
      if (name) this.benchmark(name);
    }
    this.close();
  }
}

// ----------------------------------------------------------------------------
// Flags (same names and defaults as db_bench.cc main())
// ----------------------------------------------------------------------------

function parseFlags(argv) {
  const leveldbDefaults = {
    write_buffer_size: 16 << 20,
    max_file_size: 2 << 20,
    block_size: 4096,
    open_files: 1000,
  };

  const flags = {
    benchmarks:
      'fillseq,fillsync,fillrandom,overwrite,readrandom,readrandom,readseq,' +
      'readreverse,compact,readrandom,readseq,readreverse,fill100K,stats',
    num: 100000, // db_bench uses 1000000; see the note at the top of this file
    reads: -1,
    value_size: 100,
    key_size: 16,
    compression_ratio: 0.5,
    histogram: false,
    use_existing_db: false,
    db: path.join(__dirname, 'benchmark_db'),
    ...leveldbDefaults,
    cache_size: -1,
    bloom_bits: -1,
    batch_size: 1,
    memory_profile: 'performance',
    compression: 'raw',
    compression_level: -1,
    file_write_buffer: 64 * 1024,
  };

  const numeric = {
    '--num': 'num',
    '--reads': 'reads',
    '--value_size': 'value_size',
    '--key_size': 'key_size',
    '--compression_ratio': 'compression_ratio',
    '--write_buffer_size': 'write_buffer_size',
    '--block_size': 'block_size',
    '--cache_size': 'cache_size',
    '--bloom_bits': 'bloom_bits',
    '--open_files': 'open_files',
    '--batch_size': 'batch_size',
    '--compression_level': 'compression_level',
    '--file_write_buffer': 'file_write_buffer',
  };
  const boolean = {
    '--histogram': 'histogram',
    '--use_existing_db': 'use_existing_db',
  };
  const strings = {
    '--benchmarks': 'benchmarks',
    '--db': 'db',
    '--memory_profile': 'memory_profile',
    '--compression': 'compression',
  };

  for (const arg of argv) {
    if (arg === '--help' || arg === '-h') return null;

    let matched = false;
    for (const [prefix, key] of Object.entries(strings)) {
      if (arg.startsWith(prefix + '=')) {
        flags[key] = arg.slice(prefix.length + 1);
        matched = true;
        break;
      }
    }
    if (matched) continue;

    for (const [prefix, key] of Object.entries(numeric)) {
      if (arg.startsWith(prefix + '=')) {
        const n = Number(arg.slice(prefix.length + 1));
        if (!Number.isFinite(n)) throw new Error(`bad value for ${prefix}: ${arg}`);
        flags[key] = n;
        matched = true;
        break;
      }
    }
    if (matched) continue;

    for (const [prefix, key] of Object.entries(boolean)) {
      if (arg.startsWith(prefix + '=')) {
        flags[key] = arg.slice(prefix.length + 1) === '1';
        matched = true;
        break;
      }
    }
    if (matched) continue;

    if (arg.startsWith('--max_file_size=')) {
      process.stderr.write('note: --max_file_size is not exposed by EmLDB, ignoring\n');
      continue;
    }
    if (arg.startsWith('--threads=')) {
      process.stderr.write('note: EmLDB\'s API is single threaded, --threads ignored\n');
      continue;
    }
    throw new Error(`unknown flag: ${arg}`);
  }

  if (flags.memory_profile !== 'performance' && flags.memory_profile !== 'scalable')
    throw new Error('--memory_profile must be performance or scalable');
  const compressionIds = { none: 0, zlib: 2, raw: 4, zlib_raw: 4 };
  if (!Object.hasOwn(compressionIds, flags.compression))
    throw new Error('--compression must be none, zlib, or raw');
  flags.compression_id = compressionIds[flags.compression];
  if (!Number.isInteger(flags.batch_size) || flags.batch_size < 1)
    throw new Error('--batch_size must be a positive integer');
  if (!Number.isInteger(flags.compression_level) || flags.compression_level < -1 || flags.compression_level > 9)
    throw new Error('--compression_level must be between -1 and 9');
  if (!Number.isSafeInteger(flags.num) || flags.num < 1)
    throw new Error('--num must be a positive safe integer');
  if (!Number.isSafeInteger(flags.reads))
    throw new Error('--reads must be a safe integer');
  if (!Number.isSafeInteger(flags.value_size) || flags.value_size < 1)
    throw new Error('--value_size must be a positive safe integer');
  if (!Number.isFinite(flags.compression_ratio) || flags.compression_ratio <= 0 || flags.compression_ratio > 1)
    throw new Error('--compression_ratio must be greater than 0 and at most 1');

  return flags;
}

const USAGE = `leveldb benchmark, ported from db_bench.cc

Usage: node bench.js [flags]

  --benchmarks=<list>      Comma separated list of benchmarks (default: the
                           db_bench list minus the CPU-only ones)
  --num=<n>                Keys to place in the database (default 100000;
                           db_bench uses 1000000)
  --reads=<n>              Read operations, negative means --num
  --value_size=<n>         Value size in bytes
  --key_size=<n>           Key size, used for the header estimate only
  --compression_ratio=<f>  Values are generated to shrink to this fraction
  --histogram=0|1          Print the per op latency histogram
  --use_existing_db=0|1    Do not destroy the db for fresh_db benchmarks
  --write_buffer_size=<n>  Passed to EmLDB
  --block_size=<n>         Passed to EmLDB
  --cache_size=<n>         Passed to EmLDB, negative uses its default
  --bloom_bits=<n>         Passed to EmLDB, negative uses its default
  --open_files=<n>         Passed to EmLDB
  --batch_size=<n>         Entries per write batch (fillbatch defaults to 1000)
  --memory_profile=<name>  performance (fixed heap) or scalable (growable heap)
  --file_write_buffer=<n>  Bytes buffered per writable file (default 65536).
                           Merging writes is what keeps the fill benchmarks up;
                           0 flushes on every write, which is slower but keeps
                           an acknowledged put durable across a process crash.
  --compression=<name>     none, zlib, or raw (default raw)
  --compression_level=<n>  zlib level -1..9
  --db=<path>              Database directory
  -h, --help               This message

Benchmarks: open fillseq fillbatch fillrandom overwrite fillsync fill100K
            readseq readreverse readrandom readrandomsmall readmissing
            readhot seekrandom deleteseq deleterandom compact stats sstables

Not ported from db_bench: crc32c, snappycomp, snappyuncomp, acquireload,
heapprofile (they measure leveldb's C++ helpers, not this binding) and
readwhilewriting / --threads (EmLDB's API is synchronous).
`;

async function main() {
  let flags;
  try {
    flags = parseFlags(process.argv.slice(2));
  } catch (e) {
    process.stderr.write(`${errorText(e)}\n\n${USAGE}`);
    process.exit(2);
  }
  if (flags === null) {
    process.stdout.write(USAGE);
    return;
  }

  await LevelDB.initialize({
    memory: flags.memory_profile,
    fileWriteBuffer: flags.file_write_buffer,
  });
  new Benchmark(flags).run();
}

if (require.main === module) {
  main().catch((e) => die('benchmark failed', e));
}

module.exports = { Benchmark, Stats, Histogram, Random, RandomGenerator, parseFlags };


