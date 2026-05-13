# Complex Sequence Test

## Overview

This test suite validates the ChainTree S-Expression engine's verification, waiting, and timeout functions. It runs a series of sequential tests that exercise parallel execution, event-based synchronization, time-based watchdogs, and predicate-based verification.

## Test Configuration

- **Tick Rate:** 0.1 seconds (100ms per tick)
- **Total Duration:** ~43.7 seconds (437 ticks)
- **Module:** `complex_sequence`

## Blackboard Definition

```lua
RECORD("complex_sequence_blackboard")
    FIELD("complex_sequence_condition_1", "uint32")  -- Condition flag 1
    FIELD("complex_sequence_condition_2", "uint32")  -- Condition flag 2
    FIELD("event_field", "float")                    -- Event data payload
    FIELD("field_test_counter", "uint32")            -- Loop counter
    FIELD("field_test_increment", "uint32")          -- Loop increment
    FIELD("field_test_limit", "uint32")              -- Loop limit
END_RECORD()
```

## Test Sequence

The tests run sequentially via `se_fork_join`, ensuring each test completes before the next begins.

```
se_function_interface
├── se_fork_join(wait_event_test_fn)           -- Test 1: Event waiting
├── se_fork_join(verify_time_test_fn)          -- Test 2: Time-based verification
├── se_fork_join(verify_events_test_fn)        -- Test 3: Event count verification
├── se_fork_join(complex_sequence_test_fn)     -- Test 4: Predicate verification
├── se_fork_join(wait_timeout_test_fn)         -- Test 5: Wait with timeout
└── se_return_terminate()                      -- Clean termination
```

---

## Test 1: Wait Event Test (Ticks 1-110)

**Purpose:** Validate `se_wait_event` — waiting for a specific event to occur N times.

**Structure:**
```
se_fork
├── Event Generator (se_chain_flow)
│   └── se_while loop: 10 iterations, 1.0s delay each
│       └── se_queue_event(1, 43, "event_field")
└── Event Waiter (se_chain_flow)
    └── se_wait_event(43, 10)  -- Wait for event 43 to occur 10 times
```

**Timeline:**
| Phase | Ticks | Time | Description |
|-------|-------|------|-------------|
| Start | 1 | 0.0s | Generator and waiter start |
| Iterations | 1-100 | 0-10.0s | Generator sends events every 1.0s |
| Complete | 100-110 | 10.0s | Waiter receives 10th event, completes |

**Output:**
```
event generator start
event generator iteration 1
wait event test start
...
event generator iteration 10
wait event test end
event generator end
```

---

## Test 2: Verify Time Test (Ticks 111-160)

**Purpose:** Validate `se_verify_and_check_elapsed_time` — time-based watchdog that terminates on timeout.

**Structure:**
```
se_chain_flow
├── se_log("verify time test start")
├── se_verify_and_check_elapsed_time(5.0, false, error_function)
├── se_log("waiting for termination...")
└── se_return_pipeline_continue()  -- Keep running until timeout
```

**Configuration:**
- Timeout: 5.0 seconds
- Reset flag: `false` (terminate on timeout)
- Error function: Logs "verify timeout expired this is expected"

**Timeline:**
| Phase | Ticks | Time | Description |
|-------|-------|------|-------------|
| Start | 111 | 0.0s | Verification starts, timer begins |
| Waiting | 111-159 | 0-4.9s | Monitoring elapsed time |
| Timeout | 160 | 5.0s | Timeout exceeded, error function fires, terminates |

**Output:**
```
verify time test start
waiting for termination due to elapsed time
... (50 ticks waiting) ...
verify timeout expired this is expected
```

---

## Test 3: Verify Events Test (Ticks 161-270)

**Purpose:** Validate `se_verify_and_check_elapsed_events` — event count watchdog that terminates when count exceeded.

**Structure:**
```
se_fork
├── Event Generator (se_chain_flow)
│   └── se_while loop: 10 iterations, 1.0s delay each
│       └── se_queue_event(1, 43, "event_field")
└── Event Monitor (se_chain_flow)
    └── se_verify_and_check_elapsed_events(43, 9, false, error_function)
```

**Configuration:**
- Target event: 43
- Max count: 9 (triggers on 10th event)
- Reset flag: `false` (terminate on count exceeded)

**Timeline:**
| Phase | Ticks | Time | Description |
|-------|-------|------|-------------|
| Start | 161 | 0.0s | Generator starts, monitor watches event 43 |
| Events 1-9 | 161-259 | 0-9.9s | Events counted, within limit |
| Event 10 | 260 | 10.0s | 10th event exceeds limit, error function fires |
| Complete | 270 | 11.0s | Generator finishes |

**Output:**
```
event generator start
event generator iteration 1
verify events test start
waiting for termination due to elapsed events
...
event generator iteration 10
verify timeout expired this is expected
...
event generator end
```

---

## Test 4: Complex Sequence Test (Ticks 271-336)

**Purpose:** Validate `se_wait`, `se_verify`, and the interaction between parallel condition generator and sequence verifier.

**Structure:**
```
se_fork
├── Condition Generator (se_chain_flow)
│   ├── Set (0,0) → delay 10 ticks
│   ├── Set (1,0) → delay 10 ticks
│   ├── Set (1,1) → delay 10 ticks
│   ├── Set (0,1) → delay 10 ticks  ← Triggers reset!
│   ├── Set (1,1) → delay 10 ticks
│   ├── Set (1,0) → delay 10 ticks  ← Triggers terminate!
│   └── se_return_pipeline_terminate()
└── Sequence Verifier (se_chain_flow)
    ├── se_wait(condition_1 == 1)
    ├── se_verify(condition_1 == 1, reset=true, error_fn_1)
    ├── se_wait(condition_2 == 1)
    ├── se_verify(condition_2 == 1, reset=false, error_fn_2)
    └── se_return_pipeline_continue()
```

**Condition Sequence:**
| State | Cond1 | Cond2 | Verifier Action |
|-------|-------|-------|-----------------|
| 0,0 | 0 | 0 | Waiting for cond1 |
| 1,0 | 1 | 0 | Pass test 1, wait for cond2 |
| 1,1 | 1 | 1 | Pass test 2, monitoring |
| 0,1 | 0 | 1 | **Verify fails!** Reset triggered |
| 1,1 | 1 | 1 | Pass both tests again |
| 1,0 | 1 | 0 | **Verify fails!** Terminate triggered |

**Timeline:**
| Ticks | State | Event |
|-------|-------|-------|
| 271-281 | (0,0) | Verifier waiting for condition_1 |
| 282 | (1,0) | condition_1=1, pass test 1, wait for condition_2 |
| 293 | (1,1) | condition_2=1, pass test 2, monitoring |
| 304 | (0,1) | condition_1=0, **verify fails → RESET** |
| 305-314 | (0,1) | Verifier reset, waiting for condition_1 |
| 315 | (1,1) | condition_1=1, pass test 1 & 2 quickly |
| 326 | (1,0) | condition_2=0, **verify fails → TERMINATE** |

**Output:**
```
condition generator start
condition generator set 0,0
complex sequence test start
condition generator set 1,0
complex sequence pass test 1
condition generator set 1,1
complex sequence pass test 2
waiting for error actions
condition generator set 0,1
error function 1 when high condition is expected and will produce a reset
complex sequence test start              ← Reset occurred!
condition generator set 1,1
complex sequence pass test 1
complex sequence pass test 2
waiting for error actions
condition generator set 1,0
error function 2 when low condition is expected and will produce a terminate
condition generator end
```

---

## Test 5: Wait Timeout Test (Ticks 337-437)

**Purpose:** Validate `se_wait_timeout` — waiting for a predicate with timeout protection.

**Structure:**
```
se_fork
├── Terminate Test (se_chain_flow)
│   └── se_wait_timeout(se_false(), 5.0, false, error_fn_2)
│       └── Will timeout after 5s and TERMINATE
├── Reset Test (se_chain_flow)
│   └── se_wait_timeout(se_false(), 2.0, true, error_fn_1)
│       └── Will timeout after 2s and RESET repeatedly
└── Normal Test (se_chain_flow)
    ├── se_wait_timeout(se_true(), 5.0, false, error_fn_1)
    │   └── Completes immediately (predicate true)
    ├── se_time_delay(10.0)
    └── se_return_terminate()
```

**Test Cases:**

| Test | Predicate | Timeout | Reset | Behavior |
|------|-----------|---------|-------|----------|
| Terminate | `se_false()` | 5.0s | false | Times out, terminates |
| Reset | `se_false()` | 2.0s | true | Times out, resets, repeats |
| Normal | `se_true()` | 5.0s | false | Completes immediately |

**Timeline:**
| Ticks | Time | Event |
|-------|------|-------|
| 337 | 0.0s | All three chains start |
| 356 | 2.0s | Reset test times out (1st), resets |
| 377 | 4.0s | Reset test times out (2nd), resets |
| 386 | 5.0s | Terminate test times out, terminates |
| 398 | 6.0s | Reset test times out (3rd), resets |
| 420 | 8.0s | Reset test times out (4th), resets |
| 436 | 10.0s | Normal test's time delay completes |
| 437 | 10.0s | Normal test returns TERMINATE, tree ends |

**Output:**
```
wait timeout terminate test start
wait timeout resettest start
wait normal timeout test start
... (20 ticks) ...
error function 1 when high condition is expected and will produce a reset
wait timeout resettest start    ← Reset #1
... (20 ticks) ...
error function 1 when high condition is expected and will produce a reset
wait timeout resettest start    ← Reset #2
... (8 ticks) ...
error function 2 when low condition is expected and will produce a terminate  ← 5s timeout
... (12 ticks) ...
error function 1 when high condition is expected and will produce a reset
wait timeout resettest start    ← Reset #3
... (20 ticks) ...
error function 1 when high condition is expected and will produce a reset
wait timeout resettest start    ← Reset #4
... (16 ticks) ...
ending timeout test             ← 10s delay complete
```

---

## Functions Tested

| Function | Test | Behavior Validated |
|----------|------|-------------------|
| `se_wait_event` | 1 | Wait for N occurrences of event |
| `se_verify_and_check_elapsed_time` | 2 | Timeout-based watchdog |
| `se_verify_and_check_elapsed_events` | 3 | Event count watchdog |
| `se_wait` | 4 | Wait for predicate |
| `se_verify` | 4 | Continuous predicate verification |
| `se_wait_timeout` | 5 | Wait with timeout protection |
| `se_fork` | 1,3,4,5 | Parallel execution |
| `se_fork_join` | All | Sequential test execution |
| `se_chain_flow` | All | Sequential step execution |
| `se_while` | 1,3 | Predicate-controlled loops |
| `se_queue_event` | 1,3 | Event generation |
| `se_time_delay` | 1,3,5 | Time-based delays |
| `se_tick_delay` | 4 | Tick-based delays |
| `se_field_increment_and_test` | 1,3 | Counter-based loop predicate |
| `se_field_eq` | 4 | Field equality predicate |
| `se_true` / `se_false` | 5 | Constant predicates |

## Result Summary

| Test | Duration | Result | Key Validation |
|------|----------|--------|----------------|
| Wait Event | ~11s (110 ticks) | ✅ | Event synchronization |
| Verify Time | ~5s (50 ticks) | ✅ | Time watchdog, terminate |
| Verify Events | ~11s (110 ticks) | ✅ | Event count watchdog |
| Complex Sequence | ~6.6s (66 ticks) | ✅ | Reset and terminate behavior |
| Wait Timeout | ~10s (101 ticks) | ✅ | Timeout with reset/terminate |

**Final Result:** `TERMINATE` after 437 ticks (~43.7 seconds)

## Error Function Behavior

The test validates that error functions fire correctly:

1. **Single fire on terminate:** Error function fires once before termination
2. **Repeated fire on reset:** Error function fires on each reset cycle (validated by reset test firing 4+ times)
3. **Reset clears ONESHOT flag:** `s_expr_child_reset()` is called before `s_expr_child_invoke_oneshot()` to allow repeated firing

