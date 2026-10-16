# Batched Read API Design

## Goal

Reduce JS/Wasm boundary transitions and per-result object allocation for workloads that read many keys. Support both retrieving values and checking key presence, while preserving the existing synchronous LevelDB API and its bytewise key semantics.

## Public API

```ts
getMany(keys: Array<Uint8Array | string>, options?: LevelDBReadOptions): LevelDBManyGetResult;
hasMany(keys: Array<Uint8Array | string>, options?: LevelDBReadOptions): Uint8Array;
```

`getMany` returns:

```ts
interface LevelDBManyGetResult {
  data: Uint8Array;
  offsets: Uint32Array;
  found: Uint8Array;
}
```

For input item `i`, `data.subarray(offsets[i], offsets[i + 1])` contains its value if `found[i]` is 1. A zero-length value is distinguishable from a missing key through `found`. `offsets` has `keys.length + 1` entries, starts at zero, is monotonically nondecreasing, and ends at `data.length`. Values and result buffers are owned by the caller and remain valid after later DB operations. `hasMany` returns one byte per key (1 found, 0 missing) and does not materialize values in JS.

Both APIs preserve input order and duplicate keys. Empty input returns correctly sized empty buffers without crossing into C++. Invalid key elements and a closed database throw before native work. Read options are normalized once per batch; omitted options use the DB's existing default native `ReadOptions`. LevelDB errors fail the whole operation; missing keys are normal results. The APIs are synchronous, matching existing methods.

## Architecture and Data Flow

The JS layer validates the array and key types, UTF-8 encodes strings, and packs keys into a single byte buffer plus `Uint32Array` offsets. One Embind call passes those buffers and read options to C++.

The C++ wrapper iterates the packed keys, invokes `DB::Get` per key, and appends found values into a single packed result buffer while recording presence and offsets. `hasMany` performs `DB::Get` without retaining or exporting values; LevelDB's API still needs a value output argument, but the result is discarded natively. The C++ wrapper returns packed buffers as one result object. JS converts that result to owned typed arrays once per batch.

The optimization is at the JS/Wasm boundary and result representation: it does not introduce concurrency, change LevelDB's LSM lookup order, or promise fewer underlying file reads. Cache misses still use the existing Env and `fs.readSync` path.

## Limits and Errors

Packed offsets use unsigned 32-bit values, so aggregate key or result byte length must not exceed `0xffffffff`; overflow is detected before returning corrupted offsets. Input key count must fit the typed offsets/result representation. JS validation errors identify the offending index. A not-found status sets `found[i] = 0`; other non-OK statuses raise an Error and return no partial result. Empty values set `found[i] = 1` and equal adjacent offsets.

## Testing

Tests compare each batch result with repeated `get()` calls and cover string and binary keys, arbitrary byte values, missing keys, empty values, duplicate keys, empty input, invalid arrays/keys, closed DB, and native read errors where reproducible. Type declarations are checked alongside runtime tests. A benchmark compares repeated `getView()` with `getMany()` and `hasMany()` over warm and cold cache runs, reports setup separately, and verifies the found count. Performance results are observational rather than timing assertions in CI.

## Scope

This change adds the public APIs and their type declarations, native Embind methods, packed marshalling helpers, tests, and benchmark coverage. It does not change single-key methods, database cache defaults, compression, filesystem read behavior, or unrelated benchmark scripts.
