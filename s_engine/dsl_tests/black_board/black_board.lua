-- ============================================================================
-- s_expr_tutorial.lua
-- S-Expression DSL Tutorial - Basic Record Types and Access Patterns
-- 
-- This file demonstrates:
--   1. Basic scalar field types (int32, uint32, float, int64, uint64, double)
--   2. Array types (CHAR_ARRAY, INT32_ARRAY, FLOAT32_ARRAY)
--   3. Embedded/nested records
--   4. Blackboard access (direct struct access)
--   5. Slot access (field_ref parameter access)
--   6. Array access via slots
--   7. Constant records with default values
--
-- Functions use verify pattern: write value, then verify with expected value
-- ============================================================================

local mod = start_module("black_board")

-- ============================================================================
-- SECTION 1: Basic Scalar Types
-- ============================================================================
-- Available scalar types for FIELD():
--   int32, uint32   - 32-bit integers (4 bytes, align 4)
--   int64, uint64   - 64-bit integers (8 bytes, align 8)
--   float           - 32-bit float (4 bytes, align 4)
--   double          - 64-bit float (8 bytes, align 8)
--
-- NOTE: int8, uint8, int16, uint16, bool, char are NOT allowed
--       because 32-bit writes would corrupt adjacent fields

RECORD("ScalarDemo")
    FIELD("counter", "int32")
    FIELD("flags", "uint32")
    FIELD("temperature", "float")
    FIELD("timestamp", "int64")
    FIELD("checksum", "uint64")
    FIELD("precise_value", "double")
END_RECORD()

-- ============================================================================
-- SECTION 2: Array Types
-- ============================================================================
-- CHAR_ARRAY(name, length)   - character buffer (min 4 bytes)
-- INT32_ARRAY(name, length)  - array of int32
-- FLOAT32_ARRAY(name, length) - array of float

RECORD("ArrayDemo")
    CHAR_ARRAY("name", 32)
    CHAR_ARRAY("short_tag", 4)
    INT32_ARRAY("int_values", 4)
    FLOAT32_ARRAY("float_values", 4)
END_RECORD()

-- ============================================================================
-- SECTION 3: Embedded/Nested Records
-- ============================================================================

RECORD("Vector3")
    FIELD("x", "float")
    FIELD("y", "float")
    FIELD("z", "float")
END_RECORD()

RECORD("Transform")
    FIELD("position", "Vector3")
    FIELD("rotation", "Vector3")
    FIELD("scale", "float")
END_RECORD()

-- ============================================================================
-- SECTION 4: Pointer Slots (PTR64_FIELD)
-- ============================================================================
-- Cannot be assigned from DSL - only at runtime in C code.

RECORD("LinkedNode")
    FIELD("value", "int32")
    FIELD("pad", "uint32")
    PTR64_FIELD("next", "LinkedNode")
    PTR64_FIELD("data", "void")
END_RECORD()

-- ============================================================================
-- SECTION 5: Constants (Pre-initialized Records)
-- ============================================================================

CONST("default_vector", "Vector3")
    VALUE("x", 0.0)
    VALUE("y", 1.0)
    VALUE("z", 0.0)
END_CONST()

CONST("default_transform", "Transform")
    VALUE("position.x", 0.0)
    VALUE("position.y", 0.0)
    VALUE("position.z", 0.0)
    VALUE("rotation.x", 0.0)
    VALUE("rotation.y", 0.0)
    VALUE("rotation.z", 0.0)
    VALUE("scale", 1.0)
END_CONST()

CONST("default_scalars", "ScalarDemo")
    VALUE("counter", 0)
    VALUE("flags", 0x0001)
    VALUE("temperature", 20.0)
    VALUE("timestamp", 0)
    VALUE("checksum", 0)
    VALUE("precise_value", 3.14159265358979)
END_CONST()

-- ============================================================================
-- TREE 1: Blackboard Access Demo
-- ============================================================================
-- C function casts inst->blackboard to record type and accesses directly.
-- Verify functions: write value, read back, compare with expected param.

start_tree("demo_blackboard_access")
    use_record("ScalarDemo")
    use_defaults("default_scalars")
    se_sequence( function()
    -- Write and verify counter (int32)
        -- C: rec->counter = params[0].value.i; verify against params[1].value.i
        local c1 = o_call("bb_write_verify_int32")
            int(100)    -- value to write
            int(100)    -- expected value
        end_call(c1)
        
        -- Write and verify flags (uint32)
        local c2 = o_call("bb_write_verify_uint32")
            uint(0xDEADBEEF)
            uint(0xDEADBEEF)
        end_call(c2)
        
        -- Write and verify temperature (float)
        local c3 = o_call("bb_write_verify_float")
            flt(98.6)
            flt(98.6)
        end_call(c3)
        
        -- Write and verify timestamp (int64)
        local c4 = o_call("bb_write_verify_int64")
            int(1234567890123)
            int(1234567890123)
        end_call(c4)
        
        -- Write and verify checksum (uint64)
        local c5 = o_call("bb_write_verify_uint64")
            uint(0xFEDCBA9876543210)
            uint(0xFEDCBA9876543210)
        end_call(c5)
        
        -- Write and verify precise_value (double)
        local c6 = o_call("bb_write_verify_double")
            flt(2.718281828459045)
            flt(2.718281828459045)
        end_call(c6)
    end)
end_tree("demo_blackboard_access")

-- ============================================================================
-- TREE 2: Slot Access Demo
-- ============================================================================
-- Uses field_ref() to pass field offset. Generic functions work with any field.
-- C: S_EXPR_GET_FIELD(inst, &params[0], type) for field access

start_tree("demo_slot_access")
    use_record("ScalarDemo")
    use_defaults("default_scalars")
    se_sequence(function()
        -- Write and verify via slot - int32
        local c1 = o_call("slot_write_verify_int32")
            field_ref("counter")
            int(42)     -- value to write
            int(42)     -- expected value
        end_call(c1)
        
        -- Write and verify via slot - uint32
        local c2 = o_call("slot_write_verify_uint32")
            field_ref("flags")
            uint(0xCAFEBABE)
            uint(0xCAFEBABE)
        end_call(c2)
        
        -- Write and verify via slot - float
        local c3 = o_call("slot_write_verify_float")
            field_ref("temperature")
            flt(72.5)
            flt(72.5)
        end_call(c3)
        
        -- Write and verify via slot - int64
        local c4 = o_call("slot_write_verify_int64")
            field_ref("timestamp")
            int(-9876543210)
            int(-9876543210)
        end_call(c4)
        
        -- Write and verify via slot - uint64
        local c5 = o_call("slot_write_verify_uint64")
            field_ref("checksum")
            uint(0x123456789ABCDEF0)
            uint(0x123456789ABCDEF0)
        end_call(c5)
        
        -- Write and verify via slot - double
        local c6 = o_call("slot_write_verify_double")
            field_ref("precise_value")
            flt(1.41421356237)
            flt(1.41421356237)
        end_call(c6)
    end)
end_tree("demo_slot_access")

-- ============================================================================
-- TREE 3: Array Access Demo
-- ============================================================================
-- Demonstrates CHAR_ARRAY, INT32_ARRAY, FLOAT32_ARRAY access via slots

start_tree("demo_array_access")
    use_record("ArrayDemo")
    se_sequence(function()
    -- Write and verify string in CHAR_ARRAY
        local c1 = o_call("slot_write_verify_string")
            field_ref("name")
            str("Hello, World!")
            str("Hello, World!")
        end_call(c1)
        
        -- Write and verify short string
        local c2 = o_call("slot_write_verify_string")
            field_ref("short_tag")
            str("TAG")
            str("TAG")
        end_call(c2)
        
        -- Write and verify INT32_ARRAY element by element
        local c3 = o_call("slot_write_verify_int32_element")
            field_ref("int_values")
            int(0)      -- index
            int(100)    -- value to write
            int(100)    -- expected value
        end_call(c3)
        
        local c4 = o_call("slot_write_verify_int32_element")
            field_ref("int_values")
            int(1)
            int(200)
            int(200)
        end_call(c4)
        
        local c5 = o_call("slot_write_verify_int32_element")
            field_ref("int_values")
            int(2)
            int(300)
            int(300)
        end_call(c5)
        
        local c6 = o_call("slot_write_verify_int32_element")
            field_ref("int_values")
            int(3)
            int(400)
            int(400)
        end_call(c6)
        
        -- Write and verify FLOAT32_ARRAY - bulk operation
        local c7 = o_call("slot_write_verify_float32_array")
            field_ref("float_values")
            flt(1.1)    -- values to write
            flt(2.2)
            flt(3.3)
            flt(4.4)
            flt(1.1)    -- expected values
            flt(2.2)
            flt(3.3)
            flt(4.4)
        end_call(c7)
    end)    
end_tree("demo_array_access")

-- ============================================================================
-- TREE 4: Nested Record Access Demo
-- ============================================================================
-- Uses nested_field_ref() for embedded record fields

start_tree("demo_nested_access")
    use_record("Transform")
    use_defaults("default_transform")
    se_sequence(function()
        -- Write and verify nested position.x
        local c1 = o_call("slot_write_verify_float")
            nested_field_ref("position.x")
            flt(10.0)
            flt(10.0)
        end_call(c1)
        
        -- Write and verify nested position.y
        local c2 = o_call("slot_write_verify_float")
            nested_field_ref("position.y")
            flt(20.0)
            flt(20.0)
        end_call(c2)
        
        -- Write and verify nested position.z
        local c3 = o_call("slot_write_verify_float")
            nested_field_ref("position.z")
            flt(30.0)
            flt(30.0)
        end_call(c3)
        
        -- Write and verify nested rotation.x
        local c4 = o_call("slot_write_verify_float")
            nested_field_ref("rotation.x")
            flt(45.0)
            flt(45.0)
        end_call(c4)
        
        -- Write and verify scale (not nested)
        local c5 = o_call("slot_write_verify_float")
            field_ref("scale")
            flt(2.5)
            flt(2.5)
        end_call(c5)
    end)
end_tree("demo_nested_access")

-- ============================================================================
-- TREE 5: Constant Initialization Demo
-- ============================================================================
-- Verifies that use_defaults() properly initializes blackboard from constant

start_tree("demo_constants")
    use_record("Vector3")
    use_defaults("default_vector")  -- initialized to (0, 1, 0)
    se_sequence(function()
        -- Verify default values without writing
        local c1 = o_call("slot_verify_float")
            field_ref("x")
            flt(0.0)    -- expected default
        end_call(c1)
        
        local c2 = o_call("slot_verify_float")
            field_ref("y")
            flt(1.0)    -- expected default
        end_call(c2)
        
        local c3 = o_call("slot_verify_float")
            field_ref("z")
            flt(0.0)    -- expected default
        end_call(c3)
        
        -- Modify x and verify
        local c4 = o_call("slot_write_verify_float")
            field_ref("x")
            flt(5.0)
            flt(5.0)
        end_call(c4)
        
        -- Verify y unchanged
        local c5 = o_call("slot_verify_float")
            field_ref("y")
            flt(1.0)
        end_call(c5)
    end)
end_tree("demo_constants")

return end_module(mod)