# EmLDB
<p align="center">
<img src="https://img.shields.io/npm/l/emldb?color=success" />
</p>
An Emscripten build of LevelDB with zlib compressor.

## Features
- **Multi-platform:** Easily run leveldb-mcpe on any devices supports Node.js and without build.
- **Light weight:** No dependencies, Only ~200 kB packed.
- **Close to native:** Full support of LevelDB iterator APIs.
- **High performance:** Actual disk operations can reach a sequential write speed of ~10MiB/s and a sequential read speed of ~40MiB/s.

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
> The result is being measured under `WD PC SN540 SDDPNPF-1T00`; `i7-13650HX`; `Win10 x64`; `Node.js v24.20.0`; with `emldb@2.2.2`, `node .\benchmark\bench.js --num=1000000 --block_size=163840`.

```
------------------------------------------------
DB         : E:\程序\课题性\EmLDB\test\benchmark_db
Keys:      16 bytes each
Values:    100 bytes each (50 bytes after compression)
Entries:   1000000
RawSize:   110.6 MB (estimated)
FileSize:  62.9 MB (estimated)
------------------------------------------------
EmLDB port notes: single threaded (no --threads); default --num reduced
to 100000 from db_bench's 1000000.
------------------------------------------------
fillseq      :       3.194 micros/op;   34.6 MB/s                     
fillsync     :     365.087 micros/op;    0.3 MB/s (1000 ops)          
fillrandom   :       4.914 micros/op;   22.5 MB/s                     
overwrite    :       9.586 micros/op;   11.5 MB/s                     
readrandom   :     327.974 micros/op; (1000000 of 1000000 found)      
readrandom   :     331.190 micros/op; (1000000 of 1000000 found)      
readseq      :       1.840 micros/op;   60.1 MB/s                     
readreverse  :       1.993 micros/op;   55.5 MB/s                     
compact      : 2007277.400 micros/op; (one op = the whole compaction) 
readrandom   :     336.448 micros/op; (1000000 of 1000000 found)      
readseq      :       1.726 micros/op;   64.1 MB/s                     
readreverse  :       1.838 micros/op;   60.2 MB/s                     
fill100K     :    2032.347 micros/op;   46.9 MB/s (1000 ops)          

                               Compactions
Level  Files Size(MB) Time(sec) Read(MB) Write(MB)
--------------------------------------------------
  0       17       30      1.21     0.00     36.64
  1        5        9      0.40     8.72     10.43
  2        1        2      0.06     0.00      1.74

stats        :     880.500 micros/op;  
```