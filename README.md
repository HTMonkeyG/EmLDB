# EmLDB
<p align="center">
<img src="https://img.shields.io/npm/l/emldb?color=success" />
</p>
An Emscripten build of LevelDB with zlib compressor.

## Features
- **Multi-platform:** Easily run leveldb-mcpe on any devices supports Node.js and without build.
- **Light weight:** No dependencies, Only ~200 kB packed.
- **Close to native:** Full support of LevelDB iterator APIs.

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