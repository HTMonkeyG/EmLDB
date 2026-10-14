"use strict";

const { spawnSync } = require("node:child_process");
const clean = require("./clean");

const candidates = process.env.MAKE
  ? [process.env.MAKE]
  : process.platform === "win32"
    ? ["mingw32-make", "make"]
    : ["make", "gmake"];

clean();

for (const command of candidates) {
  const result = spawnSync(command, [], { stdio: "inherit", shell: false });
  if (result.error?.code === "ENOENT") continue;
  if (result.error) throw result.error;
  process.exit(result.status ?? 1);
}

throw new Error(`No supported make executable found (tried: ${candidates.join(", ")})`);
