"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { performance } = require("node:perf_hooks");
const { LevelDB } = require("..");

const count = Number.parseInt(process.env.EMLDB_BENCH_COUNT || "10000", 10);
const valueSize = Number.parseInt(process.env.EMLDB_BENCH_VALUE_SIZE || "1024", 10);
const memory = process.env.EMLDB_MEMORY_PROFILE || "performance";

function measure(name, operation) {
  const start = performance.now();
  const result = operation();
  const milliseconds = performance.now() - start;
  console.log(JSON.stringify({
    name,
    operations: count,
    milliseconds: +milliseconds.toFixed(2),
    operationsPerSecond: Math.round(count / (milliseconds / 1000))
  }));
  return result;
}

(async () => {
  if (!Number.isSafeInteger(count) || count <= 0)
    throw new Error("EMLDB_BENCH_COUNT must be a positive integer");
  if (!Number.isSafeInteger(valueSize) || valueSize <= 0)
    throw new Error("EMLDB_BENCH_VALUE_SIZE must be a positive integer");
  if (memory !== "performance" && memory !== "scalable")
    throw new Error("EMLDB_MEMORY_PROFILE must be performance or scalable");

  await LevelDB.initialize({ memory });
  console.log(JSON.stringify({ name: "configuration", memory, count, valueSize }));
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), "emldb-bench-"));
  const dbPath = path.join(tempRoot, "db");
  const db = new LevelDB(dbPath, {
    createIfMissing: true,
    compression: LevelDB.COMPRESSIONS.ZLIB_RAW,
    bloomFilterBits: 10
  });
  const value = new Uint8Array(valueSize).fill(0x61);

  try {
    db.open();
    measure("put", () => {
      for (let index = 0; index < count; index++)
        db.put(`key-${index.toString().padStart(10, "0")}`, value);
    });
    measure("get", () => {
      for (let index = 0; index < count; index++)
        db.get(`key-${index.toString().padStart(10, "0")}`);
    });
    measure("iterate", () => {
      let visited = 0;
      for (const _entry of db) visited++;
      if (visited !== count) throw new Error(`expected ${count} entries, found ${visited}`);
    });
    console.log(JSON.stringify({
      name: "memory",
      rssMB: +(process.memoryUsage().rss / 1048576).toFixed(1),
      wasmMemory: db.getProperty("leveldb.approximate-memory-usage")
    }));
  } finally {
    if (db.db) db.close();
    try { LevelDB.destroy(dbPath, { createIfMissing: false }); } catch (_) { }
    fs.rmSync(tempRoot, { recursive: true, force: true });
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
