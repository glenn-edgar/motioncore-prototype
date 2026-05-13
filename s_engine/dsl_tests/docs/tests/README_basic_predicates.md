# Basic Primitive Test - Trigger On Change & Predicates

## Overview

This test validates the S-Engine's edge detection system (`se_trigger_on_change`) combined with composable predicates (`SE_PRED_AND`, `SE_PRED_OR`, `SE_PRED_NOT`). It demonstrates how to build reactive systems that respond to boolean state transitions.

## Test Objectives

1. **Predicate Functions** - Test user-defined predicates that read external state
2. **Composable Predicates** - Test AND, OR, NOT predicate combinators
3. **Edge Detection** - Test `se_trigger_on_change` for rising and falling edge detection
4. **Action Invocation** - Verify actions fire correctly on state transitions
5. **Repeated Triggers** - Verify actions reset and can fire again on subsequent edges

## Architecture

### Two-Variable Event System

The test uses **two separate global variables** to avoid conflicts:

```c
static uint32_t g_bitmap = 0;          // Input: test_bit predicate reads this
static uint32_t g_trigger_events = 0;  // Output: oneshot functions write this
```

**Why two variables?**

| Variable | Purpose | Who Writes | Who Reads |
|----------|---------|------------|-----------|
| `g_bitmap` | Simulated hardware/sensor state | Test harness | `test_bit` predicate |
| `g_trigger_events` | Event tracking for verification | Oneshot functions | Test harness |

```
┌─────────────────────────────────────────────────────────────────────┐
│                         TEST HARNESS                                │
│                                                                     │
│   g_bitmap = 0x01          ──────────────────────┐                 │
│   reset_trigger_events()                          │                 │
│   s_expr_node_tick(tree)                          │                 │
│   check: g_trigger_events                         │                 │
│                                                   ▼                 │
│                        ┌──────────────────────────────────────┐    │
│                        │           BEHAVIOR TREE              │    │
│                        │                                      │    │
│                        │  se_trigger_on_change                │    │
│                        │      │                               │    │
│                        │      ├─► predicate: test_bit(0)      │    │
│                        │      │       │                       │    │
│                        │      │       └─► reads g_bitmap ◄────┼────┘
│                        │      │              via user_ctx     │
│                        │      │                               │
│                        │      ├─► rising action:              │
│                        │      │       on_bit0_rise()          │
│                        │      │           │                   │
│                        │      │           └─► writes ─────────┼────┐
│                        │      │              g_trigger_events │    │
│                        │      │                               │    │
│                        │      └─► falling action:             │    │
│                        │              on_bit0_fall()          │    │
│                        │                  │                   │    │
│                        │                  └─► writes ─────────┼────┤
│                        │                     g_trigger_events │    │
│                        └──────────────────────────────────────┘    │
│                                                                     │
│   verify: g_trigger_events & EVENT_BIT0_RISE  ◄────────────────────┘
└─────────────────────────────────────────────────────────────────────┘
```

### Connection via user_ctx

The `test_bit` predicate accesses the bitmap through the tree instance's `user_ctx`:

```c
// Test setup
tree->user_ctx = &g_bitmap;

// In test_bit predicate
uint32_t* bitmap = (uint32_t*)inst->user_ctx;
bool result = (*bitmap & (1U << bit_index)) != 0;
```

## DSL Structure

### Tree Definition

```lua
start_tree("basic_primitive_test")
    
    se_function_interface(function()
        
        -- Trigger 1: Simple single bit test
        se_trigger_on_change(0,
            function()
                local pred = p_call("TEST_BIT")
                    int(0)  -- bit index
                end_call(pred)
            end,
            function()
                se_chain_flow(function()
                    local rise = o_call("ON_BIT0_RISE")
                    end_call(rise)
                    se_log("ON_BIT0_RISE")
                    se_return_continue()
                end)
            end,
            function()
                se_chain_flow(function()
                    local fall = o_call("ON_BIT0_FALL")
                    end_call(fall)
                    se_log("ON_BIT0_FALL")
                    se_return_continue()
                end)
            end
        )
        
        -- Trigger 2: AND of two bits
        se_trigger_on_change(0,
            function()
                local pred = p_call("SE_PRED_AND")
                    local p1 = p_call("TEST_BIT") int(1) end_call(p1)
                    local p2 = p_call("TEST_BIT") int(2) end_call(p2)
                end_call(pred)
            end,
            function()
                se_chain_flow(function()
                    local rise = o_call("ON_BITS_12_RISE")
                    end_call(rise)
                    se_return_continue()
                end)
            end,
            function()
                se_chain_flow(function()
                    local fall = o_call("ON_BITS_12_FALL")
                    end_call(fall)
                    se_return_continue()
                end)
            end
        )
        
        -- Trigger 3: OR of two bits
        se_trigger_on_change(0,
            function()
                local pred = p_call("SE_PRED_OR")
                    local p1 = p_call("TEST_BIT") int(3) end_call(p1)
                    local p2 = p_call("TEST_BIT") int(4) end_call(p2)
                end_call(pred)
            end,
            -- ... rising/falling actions
        )
        
        -- Trigger 4: NOT of a bit (inverted logic)
        se_trigger_on_change(1,  -- initial_state=1 (predicate starts true)
            function()
                local pred = p_call("SE_PRED_NOT")
                    local p1 = p_call("TEST_BIT") int(5) end_call(p1)
                end_call(pred)
            end,
            -- ... rising/falling actions
        )
        
        se_return_continue()
    end)
end_tree()
```

### Trigger Configuration

| Trigger | Predicate | Initial State | Rising Action | Falling Action |
|---------|-----------|---------------|---------------|----------------|
| 1 | `TEST_BIT(0)` | 0 | ON_BIT0_RISE | ON_BIT0_FALL |
| 2 | `TEST_BIT(1) AND TEST_BIT(2)` | 0 | ON_BITS_12_RISE | ON_BITS_12_FALL |
| 3 | `TEST_BIT(3) OR TEST_BIT(4)` | 0 | ON_BITS_34_RISE | ON_BITS_34_FALL |
| 4 | `NOT TEST_BIT(5)` | 1 | ON_BIT5_CLEAR | ON_BIT5_SET |

## User Functions

### Predicate: test_bit

Reads a specific bit from the bitmap via `user_ctx`:

```c
bool test_bit(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    uint32_t* bitmap = (uint32_t*)inst->user_ctx;
    int32_t bit_index = params[0].int_val;
    
    bool result = (*bitmap & (1U << bit_index)) != 0;
    return result;
}
```

### Oneshot: Event Tracking

Each oneshot sets its corresponding bit in `g_trigger_events`:

```c
void on_bit0_rise(...) {
    printf("  >> ON_BIT0_RISE\n");
    g_trigger_events |= EVENT_BIT0_RISE;
}

void on_bit0_fall(...) {
    printf("  >> ON_BIT0_FALL\n");
    g_trigger_events |= EVENT_BIT0_FALL;
}
// ... etc
```

### Event Flags

```c
#define EVENT_BIT0_RISE     (1U << 0)
#define EVENT_BIT0_FALL     (1U << 1)
#define EVENT_BITS12_RISE   (1U << 2)
#define EVENT_BITS12_FALL   (1U << 3)
#define EVENT_BITS34_RISE   (1U << 4)
#define EVENT_BITS34_FALL   (1U << 5)
#define EVENT_BIT5_CLEAR    (1U << 6)
#define EVENT_BIT5_SET      (1U << 7)
```

## Test Cases

### Test 1-2: Simple Bit Rising/Falling

```c
// Set bit 0 -> should trigger ON_BIT0_RISE
g_bitmap |= (1U << 0);
reset_trigger_events();
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
assert(get_trigger_events() & EVENT_BIT0_RISE);

// Clear bit 0 -> should trigger ON_BIT0_FALL
g_bitmap &= ~(1U << 0);
reset_trigger_events();
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
assert(get_trigger_events() & EVENT_BIT0_FALL);
```

### Test 3-5: AND Predicate

```c
// Set bit 1 only -> AND should NOT trigger
g_bitmap |= (1U << 1);
// ... no EVENT_BITS12_RISE

// Set bit 2 -> now AND is true, should trigger
g_bitmap |= (1U << 2);
// ... EVENT_BITS12_RISE fires

// Clear bit 1 -> AND becomes false
g_bitmap &= ~(1U << 1);
// ... EVENT_BITS12_FALL fires
```

### Test 6-9: OR Predicate

```c
// Set bit 3 -> OR becomes true
g_bitmap |= (1U << 3);
// ... EVENT_BITS34_RISE fires

// Set bit 4 also -> OR still true, no new trigger
g_bitmap |= (1U << 4);
// ... no event (no edge)

// Clear bit 3 -> OR still true via bit 4
g_bitmap &= ~(1U << 3);
// ... no event (no edge)

// Clear bit 4 -> OR now false
g_bitmap &= ~(1U << 4);
// ... EVENT_BITS34_FALL fires
```

### Test 10-11: NOT Predicate (Inverted Logic)

The NOT trigger starts with `initial_state=1` because `NOT bit5` is true when bit5=0:

```c
// Set bit 5 -> NOT bit5 becomes false (falling edge)
g_bitmap |= (1U << 5);
// ... EVENT_BIT5_SET fires (falling action)

// Clear bit 5 -> NOT bit5 becomes true (rising edge)
g_bitmap &= ~(1U << 5);
// ... EVENT_BIT5_CLEAR fires (rising action)
```

### Test 12-14: Repeated Triggers

Verifies that actions reset properly and can fire again:

```c
// Toggle bit 0 multiple times
for (int cycle = 0; cycle < 3; cycle++) {
    g_bitmap |= (1U << 0);
    // ... EVENT_BIT0_RISE fires each cycle
    
    g_bitmap &= ~(1U << 0);
    // ... EVENT_BIT0_FALL fires each cycle
}
```

## Expected Output

```
=== Test Trigger On Change ===
--- Initial tick (bitmap=0x00000000) ---
  events fired: 0x00
--- Set bit 0 (bitmap=0x00000000 -> 0x00000001) ---
  >> ON_BIT0_RISE
  events fired: 0x01
--- Clear bit 0 (bitmap=0x00000001 -> 0x00000000) ---
  >> ON_BIT0_FALL
  events fired: 0x02
--- Set bit 1 only (bitmap=0x00000000 -> 0x00000002) ---
  events fired: 0x00
--- Set bit 2 (bitmap=0x00000002 -> 0x00000006) ---
  >> ON_BITS_12_RISE (bit1 AND bit2)
  events fired: 0x04
...
  ✅ PASSED: All edge triggers working correctly
```

## Key Concepts Demonstrated

1. **User-Defined Predicates** - `test_bit` reads external state via `user_ctx`
2. **Composable Predicates** - `SE_PRED_AND`, `SE_PRED_OR`, `SE_PRED_NOT` combine predicates
3. **Edge Detection** - `se_trigger_on_change` detects rising and falling edges
4. **Initial State** - Configurable starting state prevents spurious triggers
5. **Action Reset** - Actions automatically reset after firing, enabling repeated triggers
6. **Two-Variable Pattern** - Separate input (bitmap) and output (events) globals

## Files

| File | Description |
|------|-------------|
| `basic_primitive_test.lua` | DSL tree definition |
| `basic_primitive_test_user_functions.c` | User predicate and oneshot implementations |
| `basic_primitive_test_user_functions.h` | Generated function prototypes |
| `basic_primitive_test_user_registration.c` | Generated registration tables |
| `basic_primitive_test.h` | Generated tree hashes |
| `basic_primitive_test_bin_32.h` | Generated binary ROM |

## Usage Pattern

This test demonstrates a common embedded systems pattern:

1. **Hardware State** → stored in `g_bitmap` (or read from actual hardware)
2. **Predicates** → evaluate hardware conditions
3. **Triggers** → detect state transitions
4. **Actions** → respond to transitions (set flags, send messages, update state)

This is useful for:
- Button debouncing and edge detection
- Sensor threshold monitoring
- Multi-condition alarm systems
- State machine input processing

