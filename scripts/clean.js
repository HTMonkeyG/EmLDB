"use strict";

const fs = require("node:fs");
const path = require("node:path");

const dist = path.resolve(__dirname, "..", "dist");
const generatedExtensions = new Set([".js", ".wasm", ".a", ".o", ".d"]);

function clean() {
  if (!fs.existsSync(dist)) return;
  for (const file of fs.readdirSync(dist)) {
    if (generatedExtensions.has(path.extname(file)))
      fs.rmSync(path.join(dist, file));
  }
}

if (require.main === module) clean();

module.exports = clean;
