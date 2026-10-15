# EmLDB

Node.js bindings for the Minecraft Bedrock-compatible `leveldb-mcpe` fork,
compiled to WebAssembly with Emscripten.

## Requirements

- Node.js 18 or newer
- A filesystem accessible to the current Node.js process
- Emscripten and GNU Make when building from source

EmLDB exposes a synchronous Node.js API backed by WebAssembly and Node filesystem
calls.

## Usage

```js
const { LevelDB } = require("emldb");

async function main() {
  await LevelDB.initialize(); // Idempotent; one Wasm runtime per process.

  const db = new LevelDB("./db", {
    createIfMissing: true,
    compression: LevelDB.COMPRESSIONS.ZLIB_RAW,
    compressionLevel: -1,
    bloomFilterBits: 10
  });

  db.open();
  try {
    db.put("foo", "bar");
    console.log(db.get("foo")); // Uint8Array [98, 97, 114]

    const batch = db.batch([
      { type: "put", key: "a", value: "one" },
      { type: "put", key: "b", value: "two" }
    ]);
    try {
      db.write(batch);
    } finally {
      batch.free();
    }

    // Breaking early releases the native iterator automatically.
    for (const [key, value] of db) {
      console.log(key, value);
      break;
    }
  } finally {
    db.close();
  }
}

main().catch(console.error);
```

Keys and values accept strings or `Uint8Array`. Strings are encoded as UTF-8;
reads always return `Uint8Array`.

## Performance tuning

EmLDB ships LevelDB's upstream option defaults. For the small-key / small-value
workloads of Minecraft Bedrock worlds and other embedded databases, those defaults
under-perform on this binding. The two levers worth setting:

- `writeBufferSize` (default 256 KiB upstream; `test/bench.js` uses `8 << 20`).
  LevelDB flushes its in-memory memtable to an SST file on every flush, and
  each write of one record crosses the wasm/JS boundary and does a synchronous
  `fs.writeSync`. A larger memtable batches far more keys per flush and per
  fsync, and on the `--benchmarks=fillseq` db_bench port that moves fillseq
  from ~5 µs/op to ~2.9 µs/op (1.7×) with 100-byte values. `16 << 20` is the
  `fillseq` knee for ~100K-key databases and also the `write-heavy` recommendation;
  bench.js uses `8 << 20` as the **balanced** default because `16 << 20` merges
  the LSM into one 5.7 MB L2 SST, making the post-fill `readseq` regress by
  ~0.6 µs/op (single large-block decompression vs. 3 small SSTs; `readseq` after
  `compact` converges). If you stay `read-heavy`, pin `writeBufferSize: 4 << 20`
  or combine `16 << 20` with `maxFileSize: 2 << 20` to keep SSTs small.
- `maxFileSize` (default 2 MiB). Upper bound for an SST file. Exposed as
  `new LevelDB(path, { maxFileSize: 2<<20 })`; use it with a large `writeBufferSize`
  to keep sequential scans fast without sacrificing fill throughput.
- `blockSize` (default 4 KiB, already LevelDB's default). 4096 matches the
  Minecraft region file page size and is a safe all-rounder; leave it alone
  unless you have long values.
- `bloomFilterBits` (default unset). A 10-bit filter only pays off when the key
  universe and working set are large enough that the block cache does not hold
  the index and filter blocks, so for typical small worlds it is neutral-to-slightly
  slower and is left off in the example above.

```js
const db = new LevelDB("./db", {
  createIfMissing: true,
  compression: LevelDB.COMPRESSIONS.ZLIB_RAW,
  writeBufferSize: 8 << 20,    // balanced default; 16<<20 for write-heavy
  maxFileSize: 2 << 20,        // keep SSTs small when writeBufferSize is large
  blockSize: 4096,
});
```

The write path is also controlled by `LevelDB.initialize({ fileWriteBuffer })`
(default `65536` bytes). A larger value means fewer `fs.writeSync` calls per
put; set it to `0` to flush every write, which is slower but keeps an
acknowledged `put` durable across a process crash.

Warm up the block cache with a full sequential scan (`for...of`) before timing
random reads — `readseq` followed by `readrandom` shows ~1.6× higher random
read throughput than cold.

## Memory profiles

- `performance` is the default. It uses a fixed 128MB Wasm heap and avoids the
  pthread memory-growth penalty.
- `scalable` starts at 32MB and can grow to 2GB. Use it for unusually large
  batches, caches, or values where a fixed heap could run out of memory.

Select the profile before the first synchronous operation:

```js
await LevelDB.initialize({ memory: "performance" });
```

One JavaScript process can initialize only one profile.

## Compression

- `NONE`: write uncompressed blocks while retaining zlib read compatibility.
- `ZLIB`: write standard zlib blocks.
- `ZLIB_RAW`: write the raw-zlib blocks used by Minecraft Bedrock. This is the
  default.
- `SNAPPY`: reserved for file-format compatibility but not included in this
  build; selecting it throws an error.

`compressionLevel` accepts `-1` (zlib default) or an integer from 0 through 9.

## Locking and crash recovery

EmLDB uses an atomically created `<database>/LOCK.emldb-lock` sidecar to keep two
EmLDB processes from opening the same database. A clean `close()` removes it; an
abrupt exit (Ctrl-C, `kill -9`, a crash) leaves it behind, and a subsequent open
recovers it automatically: EmLDB records the owning pid and hostname in the
sidecar and, on the same machine, probes `kill(pid, 0)` to confirm the owner is
gone before taking the lock over. A warning (`EMLDB_STALE_LOCK`) is emitted when
this happens. The probe is best-effort: a lock from a process on a different
machine (or an unreadable sidecar) is reported and left untouched.

Write throughput is tuned with a per-file buffer (set through
`LevelDB.initialize({ fileWriteBuffer: bytes })`, default `65536`). Buffering
fewer fs calls per put, but an acknowledged write is durable only once the buffer
reaches the OS page cache; with the default buffer, a process crash can lose the
last 64KB of writes. Use `fileWriteBuffer: 0` if your workload cannot tolerate
even that window, or pair writes with `LevelDBWriteOptions({ sync: true })`.

Native LevelDB/Minecraft processes do not know about this sidecar. Never open a
world concurrently from EmLDB and a native LevelDB-based application; pure
Node.js does not expose the advisory `flock` primitive used by native LevelDB.

Always call `close()` in a `finally` block. Garbage-collection cleanup is only a
last-resort safeguard for resources, not a durability boundary.

## Building and testing

```sh
npm test          # clean build followed by the integration suite
npm run benchmark # run the configurable I/O benchmark
```

The build emits both memory profiles under `dist/`. The test suite covers CRUD,
batching, compression modes, iterator cleanup, reopen behavior, idempotent
initialization, and cross-process locking.

JavaScript custom comparators are intentionally rejected in the threaded build:
Emscripten JavaScript handles cannot safely be invoked by LevelDB's compaction
worker. Use the default bytewise comparator.
