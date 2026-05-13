# Dictionary Subsystem Design Document

## 1. Overview

The dictionary subsystem provides two parallel APIs for navigating nested key-value structures stored in the s_engine's flat compiled parameter arrays. Both operate on the same underlying `OPEN_DICT / OPEN_KEY / CLOSE_KEY / CLOSE_DICT` and `OPEN_ARRAY / CLOSE_ARRAY` token sequences — they differ only in how keys are specified at the call site.

- **`se_dict_string`** — keys specified as dot-separated path strings, parsed at runtime. Designed for JSON-style configuration access and debugging convenience. String segments are hashed to FNV-1a at lookup time and compared against `OPEN_KEY.str_hash`.

- **`se_dict_hash`** — keys specified as pre-computed FNV-1a hash arrays, no string parsing at runtime. Designed for performance-critical paths where key hashes are known at compile time via `SE_PATH_H("key1", "key2")` macros.

Both APIs share the same dictionary token layout and provide identical typed extraction, iteration, and blackboard integration capabilities.

---

## 2. Dictionary Token Layout

Dictionaries and arrays are stored inline in the flat `s_expr_param_t` array using matched open/close tokens with `brace_idx` offsets for O(1) skip:

```
Dictionary:
  OPEN_DICT  (brace_idx → CLOSE_DICT)
    OPEN_KEY   (str_hash = FNV-1a of key name)
      <value>  (scalar, nested dict, array, or callable)
    CLOSE_KEY
    OPEN_KEY   (str_hash = ...)
      <value>
    CLOSE_KEY
  CLOSE_DICT

Array:
  OPEN_ARRAY  (brace_idx → CLOSE_ARRAY)
    <value>
    <value>
    ...
  CLOSE_ARRAY
```

Key observations:

- Keys are always stored as FNV-1a hashes in `OPEN_KEY.str_hash`, never as string pointers. Both APIs ultimately do hash-to-hash comparison.
- `brace_idx` on any open token gives the offset to the matching close token, enabling `skip_value()` to jump over nested structures in O(1).
- Values can be any param type: scalars (`INT`, `UINT`, `FLOAT`, `STR_HASH`, `STR_IDX`), nested containers (`OPEN_DICT`, `OPEN_ARRAY`), or callables (`OPEN_CALL`).

---

## 3. Path Resolution

### 3.1 String Path Resolution (`se_dicts_resolve`)

Parses a dot-separated path string one segment at a time. For each segment:

1. If current node is `OPEN_DICT` → hash the segment, call `se_dicts_find()` for hash match
2. If current node is `OPEN_ARRAY` → parse segment as numeric index, call `se_dicts_array_get()`
3. Otherwise → `TYPE_MISMATCH` error

```c
// Example: resolve "hw.gpio.mode" through nested dicts
const s_expr_param_t* val = se_dicts_resolve(dict, mod_def, "hw.gpio.mode", &ctx);
```

### 3.2 Hash Path Resolution (`se_dicth_resolve`)

Takes a pre-computed array of FNV-1a hashes and a depth count. For each level:

1. If current node is `OPEN_DICT` → call `se_dicth_find()` with hash
2. If current node is `OPEN_ARRAY` → brute-force reverse-lookup to convert hash back to numeric index, then call `se_dicth_array_get()`
3. Otherwise → `TYPE_MISMATCH` error

```c
// Example: same lookup, zero string parsing at runtime
ct_int_t mode = se_dicth_get_int(dict, SE_PATH_H("hw", "gpio", "mode"), 0);
```

### 3.3 Single-Level Lookup

Both `se_dicts_find()` and `se_dicth_find()` perform linear scan through `OPEN_KEY` tokens comparing `str_hash`. On match, they return a pointer to the value (the param immediately after `OPEN_KEY`). On mismatch, `skip_value()` jumps past the value and `CLOSE_KEY` to the next entry.

---

## 4. Typed Value Extraction

Both APIs provide identical typed getters that resolve a path then extract a value:

| Function | Returns | Coercion |
|----------|---------|----------|
| `get_int` | `ct_int_t` | INT direct, UINT/FLOAT cast |
| `get_uint` | `ct_uint_t` | UINT direct, INT/FLOAT cast |
| `get_float` | `ct_float_t` | FLOAT direct, INT/UINT cast |
| `get_bool` | `bool` | INT/UINT/FLOAT != 0 |
| `get_hash` | `s_expr_hash_t` | STR_HASH direct, STR_IDX → lookup+hash, INT/UINT as-is |
| `get_dict` | `const s_expr_param_t*` | Returns NULL if not OPEN_DICT |
| `get_array` | `const s_expr_param_t*` | Returns NULL if not OPEN_ARRAY |
| `get_callable` | `const s_expr_param_t*` | Returns NULL if not OPEN_CALL |
| `get_string` | `bool` + out params | STR_IDX only, returns index + length |
| `get_string_ptr` | `const char*` | STR_IDX → string table lookup (string API only) |

All getters return a caller-supplied default on resolution failure.

---

## 5. Iteration

Both APIs provide dictionary and array iterators with identical structure:

```c
// Dictionary iteration
se_dicth_iter_t iter;
se_dicth_iter_init(&iter, dict);
s_expr_hash_t key;
const s_expr_param_t* value;
while (se_dicth_iter_next(&iter, &key, &value)) {
    // process key-value pair
}

// Array iteration
se_arrayh_iter_t aiter;
se_arrayh_iter_init(&aiter, array);
const s_expr_param_t* elem;
uint16_t idx;
while (se_arrayh_iter_next(&aiter, &elem, &idx)) {
    // process element
}
```

Iterators track current position, end boundary, and entry index. `skip_value()` advances past each value. Both support `reset()` to restart from the beginning.

---

## 6. Blackboard Integration

Both APIs provide helpers to extract a dictionary pointer stored in a blackboard field:

```c
const s_expr_param_t* dict = se_dicth_from_instance(inst, FIELD_OFFSET_CONFIG);
ct_int_t val = se_dicth_get_int(dict, SE_PATH_H("timeout"), 1000);
```

The blackboard field stores a `uint64_t` that is cast to a `s_expr_param_t*` pointer. This allows trees to receive configuration dictionaries via blackboard binding.

---

## 7. Error Reporting

Both APIs provide a context struct for detailed error reporting:

| Field | Purpose |
|-------|---------|
| `result` | Resolved param pointer (NULL on error) |
| `status` | Status enum (OK, NOT_FOUND, TYPE_MISMATCH, INVALID_INDEX, etc.) |
| `depth` | How many levels were successfully resolved before failure |
| `failed_hash` | Hash of the segment that failed (for debugging) |
| `failed_segment_start/len` | Character offsets into path string (string API only) |

The context is optional — passing NULL skips error recording.

---

## 8. Issues Found

### 8.1 BUG: Alignment fault in `se_dicts_from_blackboard` / `se_dicth_from_blackboard`

Both headers contain:

```c
static inline const s_expr_param_t* se_dicts_from_blackboard(
    const void* blackboard, uint16_t field_offset
) {
    const uint64_t* ptr = (const uint64_t*)((const uint8_t*)blackboard + field_offset);
    return (const s_expr_param_t*)(uintptr_t)*ptr;
}
```

This casts an arbitrary `blackboard + field_offset` address to `uint64_t*` and dereferences it. On ARM Cortex-M with strict alignment requirements, if `field_offset` is not 8-byte aligned, this is a hard fault. The DSL presumably aligns pointer fields to 8 bytes in record layouts, but the C code has no guard.

**Fix:** Use `memcpy` to safely read the 8 bytes:

```c
static inline const s_expr_param_t* se_dicts_from_blackboard(
    const void* blackboard, uint16_t field_offset
) {
    if (!blackboard) return NULL;
    uint64_t val;
    memcpy(&val, (const uint8_t*)blackboard + field_offset, sizeof(val));
    return (const s_expr_param_t*)(uintptr_t)val;
}
```

### 8.2 PERFORMANCE: Hash-path array index reverse-lookup is O(256)

In `se_dicth_resolve()`, when the current node is an array:

```c
for (uint16_t idx = 0; idx < 256; idx++) {
    if (se_dicth_index_hash(idx) == path_hashes[i]) {
        index = idx;
        break;
    }
}
```

This brute-forces up to 256 hash computations to convert a hash back to a numeric index. For indices 0–15 it hits the lookup table (cheap), but 16–255 each compute a hash via digit decomposition. This is an inherent design tension — the hash-path API uses hashes uniformly, but arrays need numeric indices.

**Options:**
- Extend the lookup table to 256 entries (adds 1 KB ROM, eliminates runtime computation)
- Store array indices differently in the path (e.g., use a sentinel bit to distinguish index hashes from key hashes)
- Accept the cost — array access through hash paths is presumably rare compared to dict access

### 8.3 DEAD PARAMETER: `se_dicts_find()` accepts but ignores `module_def`

```c
const s_expr_param_t* se_dicts_find(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,  // ← unused
    const char* key,
    uint16_t key_len
) {
    (void)module_def;
```

The function hashes the key segment and compares against `OPEN_KEY.str_hash` — it never does string comparison against the string table. The `module_def` parameter is dead weight. The header comment says "Find key in dictionary by string comparison" but the implementation does hash comparison.

**Options:**
- Remove `module_def` from the signature (breaking change if external code calls it)
- Keep it for future use if string-comparison fallback on hash collision is ever needed
- At minimum, fix the header comment to say "by hash comparison"

### 8.4 CODE DUPLICATION: `skip_value()` is identically defined in both .c files

Both `se_dict_string.c` and `se_dict_hash.c` define the same `static skip_value()` function. Same with the param type check inlines and param value extraction inlines in both headers. Consider extracting to a shared `se_dict_common.h` / `se_dict_common.c`.

### 8.5 DUPLICATE FUNCTIONALITY: `se_dicts_find_hash()` duplicates `se_dicth_find()`

`se_dicts_find_hash()` in `se_dict_string.c` is functionally identical to `se_dicth_find()` in `se_dict_hash.c` — both do single-level dict lookup by hash. The string API includes it as a "fallback" but it's pure duplication.

---

## 9. Performance Characteristics

| Operation | String API | Hash API |
|-----------|-----------|----------|
| Path parsing | O(path_len) per call | Zero (pre-computed) |
| Per-level dict lookup | O(n) key scan + FNV hash of segment | O(n) key scan |
| Per-level array access | O(n) element scan + atoi parse | O(n) element scan + O(256) reverse hash |
| Skip nested value | O(1) via brace_idx | O(1) via brace_idx |
| Memory overhead | Zero allocation (read-only traversal) | Zero allocation (read-only traversal) |

Both APIs are read-only — they traverse the compiled parameter array without allocation or modification.

---

## 10. Summary

The dictionary subsystem provides two access patterns for the same underlying data: string paths for convenience and debugging, hash paths for performance. The core traversal logic is identical — linear scan through `OPEN_KEY` tokens with O(1) nested value skip via `brace_idx`. The alignment issue in the blackboard integration helpers (#8.1) is the only real bug; the rest are design observations about duplication and the hash-path array reverse-lookup cost.


