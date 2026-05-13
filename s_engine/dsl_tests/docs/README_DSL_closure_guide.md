# S-Expression DSL: Understanding `function() ... end`

## What It Is

Lua has no dedicated lambda syntax. Where other languages write:

```python
# Python
lambda: do_something()
```
```javascript
// JavaScript
() => doSomething()
```
```c
// C has no lambdas at all
```

Lua writes:

```lua
function() do_something() end
```

Every `function() ... end` in the DSL is a **deferred block** — code that is
defined now but executed later, when the engine decides to run it. This is
Lua's only mechanism for passing code as data.

## Why The DSL Requires It

The DSL builds a tree of nodes. Each node must know its children, but
children must not emit themselves until they are inside their parent's
scope. The problem:

```lua
-- WRONG: se_set_field executes immediately, BEFORE se_if_then runs
se_if_then(
    some_predicate,
    se_set_field("motor_state", STATE_IDLE)  -- emits node NOW, orphaned
)
```

```lua
-- RIGHT: closure defers emission until se_if_then calls it
se_if_then(
    some_predicate,
    function() se_set_field("motor_state", STATE_IDLE) end  -- emits when called
)
```

The closure hands **unevaluated code** to the parent node. The parent calls
it at the right moment, so the child nodes land inside the parent's scope
in the node tree.

## The Two Rules

### Rule 1: Wrap when passing code to a DSL construct

Any DSL function that takes "an action" or "a predicate" expects a closure.
The closure is called by the construct to emit child nodes.

```lua
se_if_then(
    function() ... end,   -- predicate: called to emit pred nodes
    function() ... end    -- action: called to emit action nodes
)

se_sequence(
    function() ... end    -- body: called to emit children
)

se_while(
    function() ... end,   -- condition
    function() ... end    -- body
)
```

### Rule 2: Don't wrap when calling a DSL function directly

Inside a closure body, DSL calls execute immediately — that's the point.
No extra wrapping needed.

```lua
se_sequence(function()
    se_set_field("a", 1)       -- direct call, emits node
    se_set_field("b", 2)       -- direct call, emits node
    se_log("done")             -- direct call, emits node
end)
```

## When You Need Closures vs When You Don't

### Pass a function reference directly

If you already have a zero-argument function, pass it by name.
Do NOT wrap it in another closure.

```lua
-- These are defined elsewhere as zero-argument functions:
local function monitor_thermal() ... end
local function monitor_voltage() ... end

-- GOOD: pass by reference
se_fork(
    monitor_thermal,
    monitor_voltage
)

-- REDUNDANT: wrapper does nothing
se_fork(
    function() monitor_thermal() end,
    function() monitor_voltage() end
)
```

### Use a closure to bind arguments

If the call needs arguments, there is no function reference to pass —
you need a closure to capture the arguments.

```lua
-- se_state_machine needs two arguments; no pre-bound function exists
se_fork(
    branch_serial_handler,                                     -- no args, direct ref
    function() se_state_machine("motor_state", motor_cases) end  -- has args, needs closure
)
```

### Use a closure to group multiple statements

A single DSL call is one node. Multiple calls need a container so the
parent sees one child.

```lua
-- ONE node — no container needed
se_cond_case(
    function() se_field_eq("emergency", 1) end,
    function() se_set_field("motor_state", STATE_EMERGENCY) end
)

-- TWO nodes — se_sequence groups them into one child
se_cond_case(
    function() se_field_eq("open_request", 1) end,
    function() se_sequence(function()
        se_set_field("open_request", 0)
        se_set_field("motor_state", STATE_OPENING)
    end) end
)
```

## Predicate Closures

Predicates have a subtlety. Functions like `se_field_eq` behave differently
depending on context:

- **Inside `pred_begin/pred_end`**: they register a leaf in the predicate
  builder (no closure needed around the result).
- **Outside the builder**: they **return** a closure that must be called
  to emit the p_call node.

The safest pattern for single predicates is a helper that handles this:

```lua
local function pred_field_eq(field_name, value)
    return function()
        pred_begin()
            se_field_eq(field_name, value)
        local p = pred_end()
        p()
    end
end

-- Use directly — it's already a closure
se_while(
    pred_field_eq("system_shutdown", 0),
    function() ... end
)
```

For compound predicates, build inline:

```lua
se_if_then(
    function()
        pred_begin()
            local or_id = se_pred_or()
                se_field_lt("voltage", 9.0)
                se_field_gt("voltage", 16.0)
            pred_close(or_id)
        local p = pred_end()
        p()
    end,
    function() ... end
)
```

## Quick Reference

| Situation | Pattern |
|---|---|
| Single action, no args | `function() se_set_field("x", 1) end` |
| Multiple actions | `function() se_sequence(function() ... end) end` |
| Pre-existing function, no args | `my_function` (direct reference) |
| Function call with args | `function() se_foo("a", "b") end` |
| Single predicate | `pred_field_eq("field", value)` (returns closure) |
| Compound predicate | `function() pred_begin() ... pred_end()() end` |
| Immediate execution inside body | Just call it: `se_log("msg")` |

## Common Mistakes

**Discarded return value** — predicate emits nothing:
```lua
-- WRONG: se_field_eq returns a closure, nobody calls it
se_if_then(
    function() se_field_eq("flag", 1) end,   -- closure returned and thrown away
    function() ... end
)

-- RIGHT: use pred_begin/pred_end
se_if_then(
    pred_field_eq("flag", 1),
    function() ... end
)
```

**Immediate execution** — node emits outside parent scope:
```lua
-- WRONG: se_set_field runs before se_cond_case sees it
se_cond_case(
    pred_fn,
    se_set_field("x", 1)    -- executes NOW, not when case matches
)

-- RIGHT: deferred
se_cond_case(
    pred_fn,
    function() se_set_field("x", 1) end
)
```

**Unnecessary wrapping** — adds noise, no benefit:
```lua
-- WRONG: pointless extra layer
se_fork(
    function() monitor_thermal() end
)

-- RIGHT: monitor_thermal is already a function
se_fork(
    monitor_thermal
)
```

## Summary

`function() ... end` is Lua's lambda. In this DSL it means "don't run this
now — hand it to the parent construct, which will run it at the right time
to build the node tree correctly." Use it when passing code to DSL
constructs. Don't use it when you already have a callable reference, or
when you're already inside a closure body executing directly.
