# EmLDB
<p align="center">
<img src="https://img.shields.io/npm/l/emldb?color=success" />
</p>
An Emscripten build of LevelDB with zlib compressor.

## Features
- **Multi-platform:** Easily run leveldb-mcpe on any devices supports Node.js and without build.
- **Light weight:** No dependencies, Only ~200 kB packed.
- **Close to native:** Full support of LevelDB iterator APIs.
- **Better performance:** Actual disk operations can reach a sequential write speed of ~10MiB/s and a sequential read speed of ~40MiB/s.

## Getting Started
```js
const { LevelDB } = require("emldb");

LevelDB.initialize().then(function () {
  var db = new LevelDB("./db", { createIfMissing: true, compression: LevelDB.COMPRESSIONS.ZLIB });
  
  // Open the database first.
  db.open();
  
  // Put a key-value pair into the db.
  db.put("foo", "bar");

  // Get a value from a key.
  // Uint8Array(3) [98, 97, 114]
  console.log(db.get("foo"));

  // Delete an entry in the db.
  db.delete("foo");

  // undefined
  console.log(db.get("foo"));

  db.put("foo1", "bar2");
  db.put("foo3", "bar4");
  db.put("HT", "MonkeyG");
  // JS iterator support.
  for (var kv of db) {
    console.log(kv);
  }

  // Don't forget to close the database.
  db.close();
});
```

## Benchmark
> The result is being measured under `WD PC SN540 SDDPNPF-1T00`; `i7-13650HX`; `Win10 x64`; `Node.js v24.20.0`; with `emldb@2.1.0`, `node .\benchmark\bench.js --num=1000000 --block_size=163840`.

```
RawSize:   110.6 MB (estimated)
FileSize:  62.9 MB (estimated)
------------------------------------------------
EmLDB port notes: single threaded (no --threads); default --num reduced
to 100000 from db_bench's 1000000.
------------------------------------------------
fillseq      :      11.851 micros/op;    9.3 MB/s                     
fillsync     :     375.680 micros/op;    0.3 MB/s (1000 ops)          
fillrandom   :      12.725 micros/op;    8.7 MB/s                     
overwrite    :      13.188 micros/op;    8.4 MB/s                     
readrandom   :     383.922 micros/op; (1000000 of 1000000 found)      
readrandom   :     374.922 micros/op; (1000000 of 1000000 found)      
readseq      :       3.041 micros/op;   36.4 MB/s                     
readreverse  :       3.141 micros/op;   35.2 MB/s                     
compact      : 2177375.900 micros/op; (one op = the whole compaction) 
readrandom   :     381.547 micros/op; (1000000 of 1000000 found)      
readseq      :       2.674 micros/op;   41.4 MB/s                     
readreverse  :       2.900 micros/op;   38.1 MB/s                     
fill100K     :    2443.085 micros/op;   39.0 MB/s (1000 ops)          

                               Compactions
Level  Files Size(MB) Time(sec) Read(MB) Write(MB)
--------------------------------------------------
  0       17       30      1.41     0.00     36.64
  1        5        9      0.46     8.72     10.43
  2        1        2      0.07     0.00      1.74

stats        :     744.900 micros/op;           
```