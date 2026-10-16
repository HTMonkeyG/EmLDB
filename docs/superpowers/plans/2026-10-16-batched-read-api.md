# Batched Read API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add synchronous `LevelDB.getMany()` and `LevelDB.hasMany()` APIs that batch key/value marshalling across the JS/Wasm boundary and return owned packed results.

**Architecture:** JavaScript validates keys and packs them into one contiguous byte array plus uint32 offsets. A single Embind call per batch passes the packed keys and options to C++, which loops over `DB::Get`, and returns packed results through caller-allocated Wasm output buffers. JavaScript copies the used result bytes once into caller-owned typed arrays. Calls without explicit read options reuse the DB's default options; explicit options are created once per batch. Existing single-key methods remain unchanged.

**Tech Stack:** Node.js 18+, JavaScript, TypeScript declarations, C++11, Emscripten Embind/EM_JS, Node test runner, existing LevelDB fork.

---

## File Map

- Modify `src/emldb.cc`: implement `getMany` and `hasMany` in `LevelDBWrapper`, validate packed input defensively, expose pointer/length output parameters, and register the methods with Embind.
- Modify `src/emldb_utils.h` and `src/emldb_utils.cc`: add EM_JS helpers that copy packed input/output between JS typed arrays and Wasm heap memory in one JS transition per buffer, avoiding per-key `emscripten::val` conversions.
- Modify `src/emldb.js`: pack/validate key arrays, allocate Wasm buffers and one options wrapper per batch, invoke each native method once, construct owned result arrays, and release all Wasm allocations/options in `finally` blocks.
- Modify `main.d.ts`: define `LevelDBManyGetResult`, `getMany`, and `hasMany` with ownership/layout documentation.
- Modify `test/api.test.js`: exercise public methods against a real temporary DB, including result semantics, invalid input, errors, and result lifetime.
- Modify `test/bench.js`: add `readrandombatch` and `readrandomhas` benchmarks with a dedicated read batch size flag. Keep timings informational, not CI assertions.
- Modify `README.md`: add API usage and explain packed output decoding.
- Build artifacts in `dist/` are generated/ignored; do not commit them.

## Representation and Native Contract

Public API:

```ts
getMany(keys: Array<Uint8Array | string>, options?: LevelDBReadOptions): LevelDBManyGetResult;
hasMany(keys: Array<Uint8Array | string>, options?: LevelDBReadOptions): Uint8Array;
```

Result:

```ts
export interface LevelDBManyGetResult {
  data: Uint8Array;
  offsets: Uint32Array;
  found: Uint8Array;
}
```

For `getMany`, item `i` is `data.subarray(offsets[i], offsets[i + 1])` when `found[i] === 1`; missing keys have `found[i] === 0` and equal adjacent offsets. Empty stored values have `found[i] === 1` and equal adjacent offsets. Results are owned copies and survive all later DB reads. Input order and duplicates are preserved. Empty key arrays return empty JS results without a Wasm call.

To avoid allocating a result object per key at the native boundary, JS allocates Wasm input/output buffers for packed key bytes, key offsets, packed values, value offsets, and presence bytes. One native call fills outputs and returns the used value-byte count (or throws on an error). `hasMany` reuses packed key input and one presence output buffer. EM_JS helpers perform input copies into linear memory and output copies into JS typed arrays. Native code checks DB state, offsets, output capacity, and 32-bit representability before writing. `NotFound` is a normal per-key result; any other LevelDB error aborts the call.

The API reduces JS/Wasm boundary calls; it does not parallelize reads or alter LSM/cache behavior. Native lookups remain sequential `DB::Get` calls.

## Task 1: Public Contract and Failing API Tests

**Files:**
- Modify: `test/api.test.js`
- Modify: `main.d.ts`

- [ ] **Step 1: Add a real-DB test for packed get results**

Add a `node:test` case that initializes once, creates a temporary DB, writes `a -> [1,2]`, `empty -> []`, and binary key `[0,255] -> [9]`, then queries `['a', 'missing', 'empty', binaryKey, 'a']`. Assert `data`, `offsets`, and `found` are typed arrays; `found` equals `[1,0,1,1,1]`; offsets length is `keys.length+1`, starts at 0, ends at `data.length`, and is monotonic; slices decode to expected bytes; empty value is found while missing is not; duplicate output preserves order. Assert empty input returns `data.length===0`, `offsets` `[0]`, and empty `found`. Close DB and remove temp directory in `t.after`.

- [ ] **Step 2: Run focused test and verify RED**

Run: `node --test --test-name-pattern="batch get preserves" test/api.test.js`
Expected: FAIL because `db.getMany` is not defined.

- [ ] **Step 3: Declare the public return type and methods**

Add `LevelDBManyGetResult` with fields `data: Uint8Array`, `offsets: Uint32Array`, and `found: Uint8Array`. Add `getMany(keys, options?)` and `hasMany(keys, options?)` to `LevelDB`. Document sync behavior, input order, result ownership, offset slicing, and empty/missing distinction.

## Task 2: Add Key Packing and EM_JS Buffer Copy Helpers

**Files:**
- Modify: `src/emldb_utils.h`
- Modify: `src/emldb_utils.cc`
- Modify: `src/emldb.js`
- Test: `test/api.test.js`

- [ ] **Step 1: Add failing input-validation tests**

Assert both APIs reject non-array inputs and identify invalid key elements by index. Assert string inputs are UTF-8, `Uint8Array` inputs preserve all bytes, empty key is accepted, and calls on a closed DB throw the same closed-DB error as `get()`.

- [ ] **Step 2: Run focused tests and verify RED**

Run: `node --test --test-name-pattern="batch get|batch has" test/api.test.js`
Expected: FAIL because the public batch methods do not exist.

- [ ] **Step 3: Implement JS packed-key preparation**

Add a private helper near `toUint8Array`. Require an Array; convert each element through `toUint8Array` inside a try/catch and rethrow a `TypeError` naming `keys[i]`; sum byte lengths with safe-integer and `0xffffffff` guards; allocate `Uint8Array(totalBytes)` and `Uint32Array(keys.length+1)`; copy bytes and write cumulative end offsets. Empty arrays are legal. Do not change single-key `toUint8Array` behavior.

- [ ] **Step 4: Add single-transition input and output copy helpers**

Add EM_JS helpers accepting raw Wasm pointers and lengths. The input helper copies the packed key bytes and offsets from JS-provided Uint8Array/Uint32Array into `HEAPU8`/`HEAPU32`. The output helper copies native used-byte count, offsets, and found bytes into newly allocated JS typed arrays in one invocation. Use `Emval.toHandle` for the returned aggregate and `val::take_ownership` in C++, matching existing `emJsCopyOut` ownership conventions. Keep all pointer/byte lengths checked against allocated capacities.

- [ ] **Step 5: Verify helper syntax by building the fixed and growth modules**

Run: `npm run build`
Expected: both `dist/libleveldb.js` and `dist/libleveldb-growth.js` link. Generated files remain ignored.

## Task 3: Implement Native Batch Reads

**Files:**
- Modify: `src/emldb.cc`
- Modify: `src/emldb_utils.h` / `src/emldb_utils.cc` only for the transfer helpers from Task 2.

- [ ] **Step 1: Add native method signatures and register bindings**

Add methods taking packed input pointers/count, `ReadOptionsWrapper&`, and output pointers/capacities. Register them on `class_<LevelDBWrapper>("DB")`. Keep native method call count one per batch.

- [ ] **Step 2: Implement native input validation**

Check DB is open, key offsets pointer exists, count fits `uint32_t`/`int` math, offsets start at zero, offsets are monotonic, and final key offset does not exceed packed key byte length. Validate output buffers and capacity before processing. Empty input returns immediately.

- [ ] **Step 3: Implement `getMany` with caller-provided buffers**

For each key slice, call `m_db->Get(options.get(), Slice(keyBytes+begin, end-begin), &value)`. On `IsNotFound()`, set found byte to zero and keep cumulative value offset unchanged. On OK, check output capacity and uint32 overflow before copying value bytes to the packed output buffer, set found byte to one, and advance the next offset. On other errors call `throwJsError(status.ToString())`. Preserve order and duplicates. Return total packed value-byte length.

- [ ] **Step 4: Implement `hasMany`**

Use the same key slices and lookup statuses. Store one byte for each key. Discard the native value string after each lookup. NotFound maps to zero; OK maps to one; other errors throw.

- [ ] **Step 5: Build and run the semantic test**

Run: `npm run build && node --test --test-name-pattern="batch get preserves" test/api.test.js`
Expected: both wasm profile builds succeed and semantic assertions pass.

## Task 4: Implement JS Public Methods and Resource Safety

**Files:**
- Modify: `src/emldb.js`
- Modify: `test/api.test.js`

- [ ] **Step 1: Add a failing test for explicit read options and output ownership**

Check `getMany(keys, {fillCache:false})` and `hasMany(keys, {verifyChecksums:true})` produce the same values as default options. Save a `getMany` result, perform another batch and `getView`, then assert saved `data`, `offsets`, and `found` are unchanged.

- [ ] **Step 2: Implement allocation-safe `getMany`**

Validate DB first; pack keys; return empty result immediately for zero keys. Compute safe output capacities. Allocate native input/output memory using the module's `_malloc`, write packed inputs via the EM_JS helper, call `this.db.getMany(...)` once, copy results to owned JS arrays, and free every allocation in `finally`. Construct one `LevelDBReadOptions` only for explicit options and free it in the same `finally`; omitted options use `this.defaultReadOpt`. Native output layout must include `keys.length+1` offsets and `keys.length` found bytes. Return `{data, offsets, found}`.

- [ ] **Step 3: Implement allocation-safe `hasMany`**

Follow the same validation and packed input path. Allocate one `keys.length` found output, call native once, copy to an owned Uint8Array, and release all allocations/options in `finally`. Empty input returns `new Uint8Array(0)` without native allocation.

- [ ] **Step 4: Run focused semantic, options, and ownership tests**

Run: `node --test --test-name-pattern="batch get|batch has" test/api.test.js`
Expected: PASS for all selected tests; verify no Wasm allocations are leaked across repeated calls.

## Task 5: Error Cases and Type/Runtime Compatibility

**Files:**
- Modify: `test/api.test.js`
- Modify: `main.d.ts`
- Modify: `src/emldb.js` / `src/emldb.cc` only to correct failing cases.

- [ ] **Step 1: Test single-key equivalence and edge cases**

Compare each batch entry with `db.get(key)` for mixed keys: missing key, duplicate, empty value, empty key, and high-bit bytes. Verify `hasMany[i] === (db.get(key) !== undefined)`.

- [ ] **Step 2: Test malformed inputs and native capacity errors**

Test non-array keys, invalid key at index, malformed read options, closed DB, empty batch, and explicit output capacity/overflow guards through reachable public validation. Assert errors do not poison the DB; issue a valid `getMany` after each recoverable validation failure.

- [ ] **Step 3: Run all runtime tests**

Run: `npm run test:runtime`
Expected: all existing and new tests pass, including lifecycle, compression, and process locking.

## Task 6: Add Batch Benchmarks

**Files:**
- Modify: `test/bench.js`

- [ ] **Step 1: Add `--read_batch_size` flag**

Default to 1000; parse as a safe positive integer. Keep it separate from existing `--batch_size` so fillbatch behavior does not change. Print it in the benchmark header.

- [ ] **Step 2: Add a prebuilt deterministic read-key sequence**

Build the key pool and deterministic random index sequence before `Stats.start()`. Chunk the keys by `read_batch_size`; include the last partial chunk.

- [ ] **Step 3: Add `readrandombatch` benchmark**

Call `db.getMany(chunk)` once per chunk. Sum `found` for correctness and consume `data.length`. Count logical operations as keys; report microseconds per key, not per batch.

- [ ] **Step 4: Add `readrandomhas` benchmark**

Call `db.hasMany(chunk)` and sum presence bytes. Count logical operations as keys and report microseconds per key.

- [ ] **Step 5: Run informational performance comparisons**

Run read batch sizes `1,16,64,256,1000` with warmed and reopened databases. Compare existing `readrandom`, `readrandombatch`, and `readrandomhas`; report found count and time, but do not add timing thresholds to automated tests.

## Task 7: Document and Verify

**Files:**
- Modify: `README.md`
- Modify: `main.d.ts` if review finds a missing public contract detail.

- [ ] **Step 1: Add README example**

Document `getMany` packed slicing via `data.subarray(offsets[i], offsets[i+1])`, checking `found[i]`, and `hasMany`'s one-byte output. State returned arrays own their memory and may be retained.

- [ ] **Step 2: Run all checks**

Run: `node --check main.js && node --check src/emldb.js && node --check test/bench.js && npm run test:runtime && git diff --check`.
Expected: syntax and runtime suites pass with no whitespace errors.

- [ ] **Step 3: Review scope and generated files**

Inspect `git diff --stat` and `git status --short`. Ensure generated Wasm/JS under `dist/` are not staged and existing edits/untracked files (`benchmark/io.js`, `test/bench.js`, `EmLDB.zip`, `benchmark/benchmark_db/`, `mcne_bench.exe`) are preserved.

- [ ] **Step 4: Commit only batch API implementation files**

Stage `src/emldb.cc`, `src/emldb_utils.h`, `src/emldb_utils.cc`, `src/emldb.js`, `main.d.ts`, `test/api.test.js`, `test/bench.js`, and `README.md` as changed. Do not stage unrelated changes or untracked benchmark artifacts. Commit message: `feat: add batched read APIs`.
