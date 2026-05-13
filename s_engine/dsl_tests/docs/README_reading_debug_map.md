# Reading DSL Generated Debug Dumps

## Overview

The S-Expression DSL compiler generates a debug dump file (`xxx_dump_32.h` or `xxx_dump_64.h`) that provides a human-readable view of the compiled binary tree structure. This file is invaluable for:

- Debugging tree structure issues
- Understanding parameter encoding
- Verifying function registration
- Tracing execution flow

## File Sections

### 1. Header Information

```c
/*
 * Module: state_machine_test
 * Hash:   0x6824A885
 * Trees:  1
 * Records: 1
 * Strings: 12
 * Constants: 0
 * Param size: 8 bytes (32-bit)
 */
```

| Field | Description |
|-------|-------------|
| Module | Module name from Lua DSL |
| Hash | Unique module identifier |
| Trees | Number of behavior trees |
| Records | Number of blackboard record definitions |
| Strings | Number of string literals |
| Constants | Number of constant definitions |
| Param size | Size of each parameter entry (8 bytes for 32-bit, 16 for 64-bit) |

### 2. String Table

```c
/*
 * [0x0000] (  0) hash=0xB5554AF8 "Fork Join Test Started"
 * [0x0001] (  1) hash=0x92683676 "Fork Join Test Terminated"
 * ...
 */
```

| Field | Description |
|-------|-------------|
| `[0x0000]` | String index (hex) |
| `(  0)` | String index (decimal) |
| `hash=0x...` | String hash for lookup |
| `"..."` | Actual string content |

Strings are referenced by index in `STR_IDX` parameters.

### 3. Function Tables

```c
/*
 * ONESHOT FUNCTIONS (type=0x08, with 0x40=io_call):
 *   [0x0000] ( 0) hash=0xCEBBEFA4 SE_LOG
 *   [0x0001] ( 1) hash=0xFFF84A15 SE_SET_FIELD
 *   [0x0002] ( 2) hash=0x5839B05B CFL_DISABLE_CHILDREN
 *
 * MAIN FUNCTIONS (type=0x09, with 0x80=pt_m_call):
 *   [0x0000] ( 0) hash=0xC7FEA7F6 SE_FUNCTION_INTERFACE
 *   [0x0001] ( 1) hash=0xE404E1CF SE_FORK_JOIN
 *   ...
 */
```

Three function tables:
- **ONESHOT** (type 0x08): Fire-once functions, return void
- **MAIN** (type 0x09): Persistent functions, return result codes
- **PRED** (type 0x0A): Predicate functions, return bool

Functions are referenced by index in their respective tables.

### 4. Record Definitions

```c
/*
 * RECORD[0x0000]: state_machine_blackboard (size=4, align=4, hash=0xC89D038C)
 *   [ 0] off=0x0000 size= 4 hash=0x783132F6 state
 */
```

| Field | Description |
|-------|-------------|
| `RECORD[0x0000]` | Record index |
| `state_machine_blackboard` | Record name |
| `size=4` | Total record size in bytes |
| `align=4` | Memory alignment requirement |
| `hash=0x...` | Record hash for lookup |
| `off=0x0000` | Field offset within record |
| `hash=0x783132F6` | Field hash for lookup |

### 5. Tree Parameters (Main Section)

This is the core of the dump - the flattened parameter array representing the tree structure.

## Parameter Type Codes

```c
/*
 * PARAMETER TYPE CODES:
 *   0x00 INT          0x01 UINT         0x02 FLOAT        0x03 STR_HASH
 *   0x04 SLOT         0x05 OPEN         0x06 CLOSE        0x07 OPEN_CALL
 *   0x08 ONESHOT      0x09 MAIN         0x0A PRED         0x0B FIELD
 *   0x0C RESULT       0x0D STR_IDX      0x0E CONST_REF    0x0F RESERVED
 *   0x10 OPEN_DICT    0x11 CLOSE_DICT   0x12 OPEN_KEY     0x13 CLOSE_KEY
 *   0x14 OPEN_ARRAY   0x15 CLOSE_ARRAY  0x16 OPEN_TUPLE   0x17 CLOSE_TUPLE
 *
 * FLAGS:
 *   0x40 SURVIVES_RESET (io_call, p_call_composite)
 *   0x80 POINTER        (pt_m_call)
 */
```

### Common Type Codes

| Code | Name | Description |
|------|------|-------------|
| 0x00 | INT | Signed 32-bit integer |
| 0x01 | UINT | Unsigned 32-bit integer |
| 0x02 | FLOAT | Floating point value |
| 0x06 | CLOSE | End of a callable/group |
| 0x07 | OPEN_CALL | Start of a function call |
| 0x08 | ONESHOT | Oneshot function reference |
| 0x09 | MAIN | Main function reference |
| 0x0A | PRED | Predicate function reference |
| 0x0B | FIELD | Blackboard field reference |
| 0x0D | STR_IDX | String table index |

### Flags

| Flag | Value | Description |
|------|-------|-------------|
| SURVIVES_RESET | 0x40 | Oneshot survives tree reset (io_call) |
| POINTER | 0x80 | Uses pointer-based indexing (pt_m_call) |

Combined types show both: `MAIN+PTR [0x89]` = 0x09 + 0x80

## Reading Tree Parameters

### Column Format

```
 * IDX   TYPE[CODE]       u16_a  u16_b  VALUE/DETAILS
 * -------------------------------------------------------------------------
 *    0  OPEN_CALL[0x07]    179      0  SE_FUNCTION_INTERFACE hash=0xC7FEA7F6
```

| Column | Description |
|--------|-------------|
| IDX | Parameter array index |
| TYPE | Human-readable type name |
| [CODE] | Hex type code (with flags) |
| u16_a | First 16-bit field (varies by type) |
| u16_b | Second 16-bit field (varies by type) |
| VALUE/DETAILS | Type-specific information |

### Indentation

Indentation shows nesting depth. Each level of nesting adds 2 spaces:

```
*    0  OPEN_CALL[0x07]    179      0  SE_FUNCTION_INTERFACE
*    1  MAIN      [0x09]      0      0
*    2    OPEN_CALL[0x07]      3      0  SE_LOG        <- Child of FUNCTION_INTERFACE
*    3    ONESHOT   [0x08]      2      0
*    4      STR_IDX[0x0D]        0     22              <- Parameter of SE_LOG
*    5    CLOSE[0x06]          0      -
```

### OPEN_CALL / CLOSE Pairs

Every function call is wrapped in OPEN_CALL...CLOSE:

```
*    2    OPEN_CALL[0x07]      3      0  SE_LOG hash=0xCEBBEFA4
*    3    ONESHOT   [0x08]      2      0  idx_to_ptr=0
*    4      STR_IDX[0x0D]        0     22  "Fork Join Test Started"
*    5    CLOSE[0x06]          0      -  (end SE_LOG)
```

- **OPEN_CALL u16_a**: Offset to matching CLOSE (idx 2 + 3 = idx 5)
- **OPEN_CALL u16_b**: Reserved (usually 0)
- **CLOSE**: Marks end of function call

### Function References (ONESHOT/MAIN/PRED)

```
*    3    ONESHOT   [0x08]      2      0  idx_to_ptr=0
```

- **u16_a**: Back-reference to OPEN_CALL index
- **u16_b**: Function index in the function table
- **idx_to_ptr**: Pointer table index (for pointer-based functions)

### Pointer Functions (MAIN+PTR)

```
*   13      MAIN+PTR  [0x89]     12      2  idx_to_ptr=0
```

The `+PTR` flag (0x80) indicates this function uses pointer-based instance indexing, allowing multiple concurrent instances with different parameters.

### Parameters

Parameters follow the function reference:

```
*   14        INT[0x00]            -      -  10 (0x0000000A)
```

- **INT**: Integer value, shown in decimal and hex
- **UINT**: Unsigned integer
- **FLOAT**: Floating point
- **STR_IDX**: String table reference with length
- **FIELD**: Blackboard field reference with offset and hash

### Field References

```
*   47      FIELD[0x0B]          0      4  state (off=0x0000, hash=0x783132F6)
```

- **u16_a**: Record index (0 = first record)
- **u16_b**: Field size in bytes
- **off**: Field offset within record
- **hash**: Field name hash

### String References

```
*    4      STR_IDX[0x0D]        0     22  "Fork Join Test Started"
```

- **u16_a**: String table index
- **u16_b**: String length
- **"..."**: Actual string content

## Example: Reading a State Machine Case

```
*   48      INT[0x00]            -      -  0 (0x00000000)     <- Case value
*   49      OPEN_CALL[0x07]     29      0  SE_SEQUENCE       <- Action
*   50      MAIN      [0x09]     49      5  idx_to_ptr=0
*   51        OPEN_CALL[0x07]      3      0  SE_LOG
*   52        ONESHOT   [0x08]     51      0  idx_to_ptr=0
*   53          STR_IDX[0x0D]        5      7  "State 0"
*   54        CLOSE[0x06]          0      -  (end SE_LOG)
...
*   78      CLOSE[0x06]          0      -  (end SE_SEQUENCE)
```

This shows:
1. **INT 0**: Case value for state 0
2. **SE_SEQUENCE**: The action to execute (spans indices 49-78)
3. **SE_LOG "State 0"**: First child of the sequence

## Tracing Execution

To trace how a tree executes:

1. Find the root `OPEN_CALL` (index 0)
2. Note the function type (MAIN for composites)
3. Follow children (parameters between OPEN_CALL and CLOSE)
4. Each `OPEN_CALL` child is a nested function call

### Example Trace

```
SE_FUNCTION_INTERFACE (0-179)
├── SE_LOG "Fork Join Test Started" (2-5)
├── SE_FORK_JOIN (6-20)
│   ├── SE_LOG "Fork Join Test Started" (8-11)
│   ├── SE_TICK_DELAY 10 (12-15)
│   └── SE_LOG "Fork Join Test Terminated" (16-19)
├── SE_FORK (21-35)
│   └── ...
├── SE_SET_FIELD state=0 (36-40)
├── SE_LOG "State machine test started" (41-44)
├── SE_STATE_MACHINE (45-167)
│   ├── FIELD state
│   ├── INT 0, SE_SEQUENCE (state 0 case)
│   ├── INT 1, SE_SEQUENCE (state 1 case)
│   ├── INT 2, SE_SEQUENCE (state 2 case)
│   └── INT -1, SE_SEQUENCE (default case)
├── SE_TICK_DELAY 350 (168-171)
├── SE_LOG "State machine test finished" (172-175)
└── SE_RETURN_FUNCTION_TERMINATE (176-178)
```

## Debugging Tips

### Finding a Function

Search for the function name hash to find all invocations:

```bash
grep "SE_TICK_DELAY" xxx_dump_32.h
```

### Checking Nesting

Count OPEN_CALL vs CLOSE to verify balanced structure:

```bash
grep -c "OPEN_CALL" xxx_dump_32.h  # Should equal...
grep -c "CLOSE\[0x06\]" xxx_dump_32.h
```

### Verifying Parameters

Check that parameter types match expected function signatures:

- `SE_TICK_DELAY` should have INT parameter
- `SE_LOG` should have STR_IDX parameter
- `SE_STATE_MACHINE` should have FIELD then [INT, action] pairs

### Finding External Functions

External (user-defined) functions appear in the function table but with user-defined hashes:

```
*   [0x0002] ( 2) hash=0x5839B05B CFL_DISABLE_CHILDREN  <- User function
```

These require implementation in `user_functions.c`.

