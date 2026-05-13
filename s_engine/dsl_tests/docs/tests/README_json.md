# JSON Dictionary Extract Test

Comprehensive test for the S-Engine dictionary extraction system. Exercises both string-path and hash-path APIs across five passes, verifying 36 assertions covering all supported value types, nested access, array indexing, and sub-dictionary pointer storage.

## Files

- `json_extract_test.lua` — DSL test definition (tree, record, five passes)
- `user_dict_extract_debug.c` — C user functions for formatted output and verification

## Test Configuration

The test loads a Lua table as both a string-keyed dictionary (`dict_string`) and a hash-keyed dictionary (`dict_hash`). The configuration contains:

- **integers** — positive (12345), negative (-9876), zero, and nested 4-deep (42)
- **unsigned** — small (100), medium (50000), large (0xFFFF), nested (255)
- **floats** — pi (3.14159), negative (-273.15), zero, nested (2.71828)
- **bools** — true (1), false (0), nested (1)
- **hashes** — string values hashed via FNV-1a: "idle", "running", "error", nested "deep_hash"
- **int_array** — `{10, 20, 30, 40}`
- **float_array** — `{1.5, 2.5, 3.5}`
- **items** — array of dicts: `[{id:100, value:10.1}, {id:200, value:20.2}, {id:300, value:30.3}]`

## Record Layout

The `extract_state` record holds all test fields on the blackboard:

| Field | Type | Purpose |
|-------|------|---------|
| `dict_string`, `dict_hash` | PTR64 | Dictionary pointers loaded at startup |
| `pass_number` | uint32 | Incremented each pass |
| `int_val_1..3` | int32 | Scalar integer extractions |
| `uint_val_1..3` | uint32 | Scalar unsigned extractions |
| `float_val_1..3` | float | Scalar float extractions |
| `bool_val_1..3` | uint32 | Boolean extractions (0/1) |
| `hash_val_1..3` | uint32 | FNV-1a hash extractions |
| `arr_int_0..3` | int32 | Integer array elements |
| `arr_float_0..2` | float | Float array elements |
| `arr_nested_N_id`, `arr_nested_N_val` | uint32/float | Nested array dict fields |
| `sub_integers`, `sub_floats` | PTR64 | Pointers to sub-dictionaries |
| `sub_nested_0`, `sub_nested_1` | PTR64 | Pointers to array element dicts |
| `ptr_int_pos`, `ptr_int_neg` | int32 | Extracted via sub-dict pointer |
| `ptr_float_pi`, `ptr_float_neg` | float | Extracted via sub-dict pointer |
| `ptr_n0_id`, `ptr_n0_val`, `ptr_n1_id`, `ptr_n1_val` | uint32/float | Extracted via array element pointer |

## Five Passes

### Pass 1 — String Path Extraction

Extracts values from `dict_string` using dot-separated string paths. Covers all five value types (int, uint, float, bool, hash) including nested paths up to 4 levels deep.

```lua
se_dict_extract_int("dict_string", "integers.positive", "int_val_1")
se_dict_extract_int("dict_string", "integers.nested.deep.value", "int_val_3")
```

**15 assertions:** 3 int, 3 uint, 3 float, 3 bool, 3 hash

### Pass 2 — Hash Path Extraction

Clears all scalar fields, then repeats the same extractions from `dict_hash` using hash-path arrays. Verifies the hash-based API produces identical results.

```lua
se_dict_extract_int_h("dict_hash", {"integers", "positive"}, "int_val_1")
se_dict_extract_int_h("dict_hash", {"integers", "nested", "deep", "value"}, "int_val_3")
```

**15 assertions:** same value types as Pass 1

### Pass 3 — Array Element Access

Extracts from arrays using numeric index segments in dot-paths. Tests integer arrays, float arrays, and arrays of dictionaries.

```lua
se_dict_extract_int("dict_string", "int_array.0", "arr_int_0")
se_dict_extract_uint("dict_string", "items.0.id", "arr_nested_0_id")
se_dict_extract_float("dict_string", "items.2.value", "arr_nested_2_val")
```

**13 assertions:** 4 int array, 3 float array, 6 nested dict fields (3 items × id + value)

### Pass 4 — String-Path Pointer Storage and Extraction

Stores pointers to sub-dictionaries and array elements in PTR64 blackboard fields, then extracts values through those pointers using string paths.

```lua
se_dict_store_ptr("dict_string", "integers", "sub_integers")
se_dict_store_ptr("dict_string", "items.0", "sub_nested_0")
-- Then extract through the pointer:
se_dict_extract_int("sub_integers", "positive", "ptr_int_pos")
se_dict_extract_uint("sub_nested_0", "id", "ptr_n0_id")
```

**8 assertions:** 2 int, 2 float, 2 nested id, 2 nested value

### Pass 5 — Hash-Path Pointer Storage and Extraction

Clears pointer result fields, then repeats the pointer storage and extraction using hash-path APIs. Verifies hash-based pointer resolution works for both sub-dictionaries and array elements.

```lua
se_dict_store_ptr_h("dict_hash", {"items", "0"}, "sub_nested_0")
se_dict_extract_uint_h("sub_nested_0", {"id"}, "ptr_n0_id")
```

**8 assertions:** same values as Pass 4, verified via `USER_VERIFY_RESULTS`

Note: Pass 5 overwrites the same result fields as Pass 4. The final verification checks the Pass 5 (hash-path) results.

## User Functions (C)

Four oneshot functions registered in the function table:

### USER_PRINT_EXTRACT_RESULTS

Prints formatted results for Pass 1 and Pass 2 scalar extractions. Receives 17 params: title string, pass number, and 15 field references covering int, uint, float, bool, and hash values with expected values shown inline.

### USER_PRINT_ARRAY_RESULTS

Prints formatted results for Pass 3 array access. Receives 15 params: title, pass number, 4 int array fields, 3 float array fields, and 6 nested array dict fields.

### USER_PRINT_POINTER_RESULTS

Prints formatted results for Pass 4 and Pass 5 pointer extractions. Receives 10 params: title, pass number, 2 int fields, 2 float fields, and 4 nested array element fields.

### USER_VERIFY_RESULTS

Final automated verification across all passes. Takes no params — reads the blackboard directly using `s_expr_blackboard_get_*` with field name hashes. Runs 36 checks using `check_int`, `check_uint`, `check_float`, and `check_hash` helpers with pass/fail output. Returns total pass/fail count.

### Verification Helpers

| Helper | Comparison |
|--------|------------|
| `check_int(name, got, expected, &errors)` | Exact int32 equality |
| `check_uint(name, got, expected, &errors)` | Exact uint32 equality |
| `check_float(name, got, expected, &errors)` | `fabsf(got - expected) < 0.01` |
| `check_hash(name, got, str, &errors)` | `got == s_expr_hash(str)` |

## Expected Output (passing)

```
║  ✅ ALL 36 TESTS PASSED
```

## API Coverage

| API Function | Passes Used |
|-------------|-------------|
| `se_load_dictionary` | Setup |
| `se_load_dictionary_hash` | Setup |
| `se_dict_extract_int` | 1, 3 |
| `se_dict_extract_uint` | 1, 3 |
| `se_dict_extract_float` | 1, 3 |
| `se_dict_extract_bool` | 1 |
| `se_dict_extract_hash` | 1 |
| `se_dict_extract_int_h` | 2, 5 |
| `se_dict_extract_uint_h` | 2, 5 |
| `se_dict_extract_float_h` | 2, 5 |
| `se_dict_extract_bool_h` | 2 |
| `se_dict_extract_hash_h` | 2 |
| `se_dict_store_ptr` | 4 |
| `se_dict_store_ptr_h` | 5 |
