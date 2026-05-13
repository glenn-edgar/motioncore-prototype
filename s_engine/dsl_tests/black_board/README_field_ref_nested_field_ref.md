# field_ref vs nested_field_ref

The difference is how they resolve field paths in the DSL.

## field_ref

Accesses a **direct field** of the current record:

```lua
use_record("ScalarDemo")

field_ref("counter")      -- ScalarDemo.counter
field_ref("temperature")  -- ScalarDemo.temperature
```

Only works for top-level fields in the bound record.

## nested_field_ref

Accesses a **field within an embedded record** using dot notation:

```lua
use_record("Transform")
-- Transform contains: position (Vector3), rotation (Vector3), scale (float)
-- Vector3 contains: x, y, z

nested_field_ref("position.x")   -- Transform.position.x
nested_field_ref("rotation.y")   -- Transform.rotation.y
field_ref("scale")               -- Transform.scale (not nested, use field_ref)
```

## Under the Hood

Both produce a `S_EXPR_PARAM_FIELD` parameter with `field_offset` and `field_size`. The difference is just how the DSL calculates the offset:

| Function | Offset Calculation |
|----------|-------------------|
| `field_ref("x")` | `record.fields["x"].offset` |
| `nested_field_ref("position.x")` | `record.fields["position"].offset + Vector3.fields["x"].offset` |

At runtime, C code uses the same macro for both:

```c
float* field = S_EXPR_GET_FIELD(inst, &params[0], float);
```

## When to Use Each

| Situation | Use |
|-----------|-----|
| Simple record with no embedded records | `field_ref()` |
| Top-level field in any record | `field_ref()` |
| Field inside an embedded record | `nested_field_ref()` |

## Example Record Definitions

```lua
RECORD("Vector3")
    FIELD("x", "float")
    FIELD("y", "float")
    FIELD("z", "float")
END_RECORD()

RECORD("Transform")
    FIELD("position", "Vector3")   -- embedded record
    FIELD("rotation", "Vector3")   -- embedded record
    FIELD("scale", "float")        -- simple field
END_RECORD()
```

## Example Usage in Trees

```lua
start_tree("example")
    use_record("Transform")
    
    -- Access nested fields
    local c1 = o_call("slot_write_verify_float")
        nested_field_ref("position.x")  -- offset = 0
        flt(10.0)
        flt(10.0)
    end_call(c1)
    
    local c2 = o_call("slot_write_verify_float")
        nested_field_ref("position.y")  -- offset = 4
        flt(20.0)
        flt(20.0)
    end_call(c2)
    
    local c3 = o_call("slot_write_verify_float")
        nested_field_ref("rotation.x")  -- offset = 12 (after Vector3 position)
        flt(45.0)
        flt(45.0)
    end_call(c3)
    
    -- Access top-level field
    local c4 = o_call("slot_write_verify_float")
        field_ref("scale")              -- offset = 24 (after two Vector3s)
        flt(2.5)
        flt(2.5)
    end_call(c4)
    
end_tree("example")
```

## Memory Layout

For the `Transform` record:

```
Offset  Field              Size
------  -----------------  ----
0       position.x         4
4       position.y         4
8       position.z         4
12      rotation.x         4
16      rotation.y         4
20      rotation.z         4
24      scale              4
------
Total: 28 bytes
```

- `field_ref("position")` → offset=0, size=12 (entire Vector3)
- `nested_field_ref("position.x")` → offset=0, size=4 (just x)
- `nested_field_ref("rotation.y")` → offset=16, size=4
- `field_ref("scale")` → offset=24, size=4