SE_WAIT
Overview
se_wait is a blocking synchronization function that halts execution until a predicate becomes true. It provides a simple mechanism for waiting on conditions, events, or state changes.
Function Type

Type: MAIN (m_call)
Storage: None (stateless)
Blocking: Yes (returns SE_PIPELINE_HALT while waiting)

Parameters
IndexTypeDescription0PREDPredicate function to evaluate
Lua DSL
luase_wait(pred_function)
```

### Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `pred_function` | function | Predicate to evaluate each tick (must return boolean) |

### Validation

- `pred_function` must be a function

## Behavior

### State Machine
```
┌─────────────────────────────────────────────────────┐
│                      INIT                           │
│  • Validate parameters                              │
│  • Return SE_PIPELINE_CONTINUE                      │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│                    WAITING                          │
│  • Evaluate predicate each tick                     │
│  • If predicate == false: SE_PIPELINE_HALT          │
│  • If predicate == true: SE_PIPELINE_DISABLE        │
└─────────────────────────────────────────────────────┘
                         │
                         ▼ (predicate becomes true)
┌─────────────────────────────────────────────────────┐
│                    COMPLETE                         │
│  • Return SE_PIPELINE_DISABLE                       │
└─────────────────────────────────────────────────────┘
Event Handling
Event TypeBehaviorSE_EVENT_INITValidate parametersSE_EVENT_TERMINATEReturn SE_PIPELINE_CONTINUE (no cleanup)SE_EVENT_TICKEvaluate predicateOther eventsEvaluate predicate
Return Codes
ConditionReturnsPredicate is falseSE_PIPELINE_HALT (still waiting)Predicate is trueSE_PIPELINE_DISABLE (complete)
Usage Examples
Wait for Field Condition
luase_chain_flow(function()
    se_log("Waiting for counter to reach 10...")
    se_wait(se_field_ge("counter", 10))
    se_log("Counter reached 10!")
    se_return_pipeline_disable()
end)
Wait for External Signal
luase_chain_flow(function()
    se_log("Waiting for sensor ready...")
    se_wait(se_pred("SENSOR_READY"))
    se_log("Sensor is ready!")
    se_do_sensor_work()
    se_return_pipeline_disable()
end)
Wait for Flag
luase_chain_flow(function()
    se_log("Waiting for initialization complete...")
    se_wait(se_field_eq("initialized", 1))
    se_log("System initialized!")
    se_return_pipeline_disable()
end)
Sequential Waits
luase_chain_flow(function()
    se_log("Step 1: Wait for connection")
    se_wait(se_pred("CONNECTED"))
    
    se_log("Step 2: Wait for authentication")
    se_wait(se_field_eq("authenticated", 1))
    
    se_log("Step 3: Wait for data ready")
    se_wait(se_field_gt("data_count", 0))
    
    se_log("All conditions met!")
    se_return_pipeline_disable()
end)
Wait with Complex Predicate
luase_chain_flow(function()
    -- Build complex predicate
    pred_begin()
        local and1 = se_pred_and()
            se_field_eq("mode", MODE_READY)
            se_field_gt("level", 0)
            se_pred("HARDWARE_OK")
        pred_close(and1)
    local ready_condition = pred_end()
    
    se_log("Waiting for system ready...")
    se_wait(ready_condition)
    se_log("System ready!")
    
    se_return_pipeline_disable()
end)
Producer-Consumer Pattern
luase_fork(function()
    -- Producer
    se_chain_flow(function()
        se_while(se_state_increment_and_test(1, 10), function()
            se_log("Producing item...")
            se_increment_field("item_count", 1)
            se_time_delay(0.5)
        end)
        se_log("Producer done")
        se_return_pipeline_disable()
    end)
    
    -- Consumer
    se_chain_flow(function()
        se_while(se_state_increment_and_test(1, 10), function()
            se_wait(se_field_gt("item_count", 0))
            se_log("Consuming item...")
            se_decrement_field("item_count", 1)
        end)
        se_log("Consumer done")
        se_return_pipeline_disable()
    end)
end)
Wait with Timeout (using se_fork)
luase_fork(function()
    -- Wait for condition
    se_chain_flow(function()
        se_wait(se_pred("RESPONSE_RECEIVED"))
        se_set_field("got_response", 1)
        se_return_pipeline_terminate()  -- Kill timeout
    end)
    
    -- Timeout
    se_chain_flow(function()
        se_time_delay(5.0)
        se_set_field("timed_out", 1)
        se_return_pipeline_terminate()  -- Kill waiter
    end)
end)
Gated Execution
luase_chain_flow(function()
    -- Wait for permission
    se_wait(se_field_eq("permission_granted", 1))
    
    -- Execute protected operation
    se_log("Permission granted - executing")
    se_do_protected_operation()
    
    -- Clear permission for next time
    se_set_field("permission_granted", 0)
    
    se_return_pipeline_disable()
end)
Comparison with Related Functions
FunctionWaits ForOn SuccessOn FailureBlockingse_waitPredicate trueDISABLEHALT (keep waiting)Yesse_wait_eventEvent N timesDISABLEHALT (keep waiting)Yesse_verifyPredicate trueCONTINUERESET/TERMINATENose_tick_delayN ticksDISABLEN/AYesse_time_delayElapsed timeDISABLEN/AYes
Key Differences from se_verify
Aspectse_waitse_verifyPurposeWait for conditionAssert invariantOn trueComplete (DISABLE)Continue monitoringOn falseKeep waiting (HALT)Trigger errorError handlerNoneRequiredUse caseSynchronizationGuard condition
Predicate Functions
se_wait works with any predicate function:
Field Comparisons
luase_wait(se_field_eq("state", 1))      -- field == value
se_wait(se_field_ne("error", 0))      -- field != value
se_wait(se_field_gt("count", 10))     -- field > value
se_wait(se_field_ge("level", 5))      -- field >= value
se_wait(se_field_lt("temp", 100))     -- field < value
se_wait(se_field_le("usage", 80))     -- field <= value
se_wait(se_field_in_range("x", 0, 100))  -- min <= field <= max
Custom Predicates
luase_wait(se_pred("SENSOR_READY"))      -- User-defined predicate
se_wait(se_pred("MOTOR_STOPPED"))
Event Checking
luase_wait(se_check_event(USER_EVENT_1)) -- Wait for specific event
Boolean Constants
luase_wait(se_true())   -- Returns immediately (always true)
se_wait(se_false())  -- Waits forever (always false) - DON'T DO THIS!
Composite Predicates
luapred_begin()
    local or1 = se_pred_or()
        se_field_eq("mode", 1)
        se_field_eq("mode", 2)
    pred_close(or1)
local multi_mode = pred_end()

se_wait(multi_mode)
C Implementation Notes
Stateless Design
The function uses m_call (not pt_m_call) because it doesn't need persistent storage — it simply evaluates the predicate each tick.
Event Processing
Unlike se_verify, se_wait processes all events (not just SE_EVENT_TICK):
c// No event_id filtering
bool pred_result = s_expr_child_invoke_pred(inst, params, param_count, 0);
This allows se_wait to respond to user events if the predicate checks for them.
Predicate Invocation
cbool pred_result = s_expr_child_invoke_pred(inst, params, param_count, 0);
The predicate is at logical child index 0.
Error Handling
ErrorTriggerparam_count < 1Missing predicate parameter
Exceptions halt execution (Erlang-style fail-fast).
Notes

se_wait is a fundamental synchronization primitive in ChainTree
It blocks the pipeline (returns SE_PIPELINE_HALT) until the condition is met
The predicate is evaluated on every tick, including user events
There is no built-in timeout — use se_fork with se_time_delay for timeout behavior
Waiting on se_false() will wait forever (infinite loop)
The function is stateless and can be safely reset/restarted
When used in se_chain_flow, subsequent children won't execute until se_wait completes

