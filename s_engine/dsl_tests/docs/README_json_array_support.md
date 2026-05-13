## Array Handling

### Array Structure in Binary Format

Arrays are stored as sequential values between `OPEN_ARRAY` and `CLOSE_ARRAY` tokens:
```
OPEN_ARRAY [brace_idx=N]
  INT [value=10]
  INT [value=20]
  INT [value=30]
  FLOAT [value=3.14]
  OPEN_DICT [brace_idx=M]
    ...nested dict...
  CLOSE_DICT
CLOSE_ARRAY
```

### DSL Array Syntax

Arrays in Lua tables are compiled directly:
```lua
local config = {
    -- Simple array of integers
    thresholds = {10, 20, 30, 40, 50},
    
    -- Array of floats
    calibration = {1.0, 1.5, 2.0, 2.5},
    
    -- Mixed types (valid but less common)
    mixed = {42, 3.14, "label"},
    
    -- Array of dictionaries
    zones = {
        {id = 1, name = "north", enabled = 1},
        {id = 2, name = "south", enabled = 0},
        {id = 3, name = "east", enabled = 1},
    },
    
    -- Nested arrays
    matrix = {
        {1, 2, 3},
        {4, 5, 6},
        {7, 8, 9},
    }
}
```

### Accessing Arrays via Path

Arrays can be accessed by index using numeric path segments:
```lua
-- Access array element by index (0-based in path)
se_dict_extract_int("config_ptr", "thresholds.0", "thresh_0")
se_dict_extract_int("config_ptr", "thresholds.1", "thresh_1")
se_dict_extract_int("config_ptr", "thresholds.2", "thresh_2")

-- Access nested dict in array
se_dict_extract_int("config_ptr", "zones.0.id", "zone0_id")
se_dict_extract_int("config_ptr", "zones.1.id", "zone1_id")
se_dict_extract_hash("config_ptr", "zones.0.name", "zone0_name_hash")

-- Access nested array element
se_dict_extract_int("config_ptr", "matrix.1.2", "matrix_1_2")  -- row 1, col 2 = 6
```

### Hash Path Array Access

For hash paths, use `SE_IDXH(n)` macro or numeric strings:
```lua
-- Using index in hash path
se_dict_extract_int_h("config_ptr", {"thresholds", "0"}, "thresh_0")
se_dict_extract_int_h("config_ptr", {"zones", "0", "id"}, "zone0_id")
se_dict_extract_int_h("config_ptr", {"matrix", "1", "2"}, "matrix_1_2")
```

In C code:
```c
// Using SE_IDXH macro for array indices
ct_int_t val = se_dicth_get_int(dict, 
    (s_expr_hash_t[]){s_expr_hash("thresholds"), SE_IDXH(0)}, 2, 0);

// Or use SE_PATH macros
ct_int_t val = se_dicth_get_int(dict, SE_PATH_H("zones", "0", "id"), 0);
```

### Runtime Array Iteration

#### String-Path Library (se_dict_string.h)
```c
// Get array from path
const s_expr_param_t* array = se_dicts_get_array(dict, mod_def, "zones");
if (!array) return;

// Get array element count
uint16_t count = se_dicts_array_count(array);
printf("Array has %d elements\n", count);

// Iterate array elements
se_arrays_iter_t iter;
se_arrays_iter_init(&iter, array);

const s_expr_param_t* value;
uint16_t index;

while (se_arrays_iter_next(&iter, &value, &index)) {
    // value points to current element
    // index is 0-based position
    
    uint8_t opcode = value->type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_INT) {
        printf("[%d] = %d\n", index, (int)value->int_val);
    } 
    else if (opcode == S_EXPR_PARAM_OPEN_DICT) {
        // Element is a nested dictionary - can navigate further
        ct_int_t id = se_dicts_get_int(value, mod_def, "id", -1);
        printf("[%d].id = %d\n", index, (int)id);
    }
}

// Reset iterator to start over
se_arrays_iter_reset(&iter);
```

#### Hash-Path Library (se_dict_hash.h)
```c
// Get array from path
const s_expr_param_t* array = se_dicth_get_array(dict, SE_PATH_H("zones"));
if (!array) return;

// Get element count
uint16_t count = se_dicth_array_count(array);

// Get specific element by index
const s_expr_param_t* elem = se_dicth_array_get(array, 0);  // First element

// Iterate array
se_arrayh_iter_t iter;
se_arrayh_iter_init(&iter, array);

const s_expr_param_t* value;
uint16_t index;

while (se_arrayh_iter_next(&iter, &value, &index)) {
    if (se_dicth_is_dict(value)) {
        // Navigate into nested dict using hash path
        ct_int_t id = se_dicth_get_int(value, SE_PATH_H1("id"), 0);
        printf("[%d].id = %d\n", index, (int)id);
    }
}
```

### Built-in Array Helpers

The standard helpers extract single values. For arrays, you have several options:

#### Option 1: Extract Elements Individually
```lua
RECORD("sensor_config")
    PTR64_FIELD("config_ptr", "void")
    FIELD("cal_0", "float")
    FIELD("cal_1", "float")
    FIELD("cal_2", "float")
    FIELD("cal_3", "float")
END_RECORD()

se_sequence(function()
    se_load_dictionary("config_ptr", config)
    se_dict_extract_float("config_ptr", "calibration.0", "cal_0")
    se_dict_extract_float("config_ptr", "calibration.1", "cal_1")
    se_dict_extract_float("config_ptr", "calibration.2", "cal_2")
    se_dict_extract_float("config_ptr", "calibration.3", "cal_3")
end)
```

#### Option 2: Use Bulk Extraction
```lua
se_dict_extract_all("config_ptr", {
    {path = "calibration.0", dest = "cal_0", type = "float"},
    {path = "calibration.1", dest = "cal_1", type = "float"},
    {path = "calibration.2", dest = "cal_2", type = "float"},
    {path = "calibration.3", dest = "cal_3", type = "float"},
})
```

#### Option 3: Create Custom Array Handler

For dynamic or large arrays, create a custom handler:

**DSL Helper:**
```lua
function se_dict_extract_float_array(dict_field, array_path, base_field, count)
    validate_field_is_ptr64(dict_field)
    
    local call = o_call("USER_EXTRACT_FLOAT_ARRAY")
        field_ref(dict_field)
        str(array_path)
        field_ref(base_field)  -- First field in contiguous block
        int(count)
    end_call(call)
end
```

**C Implementation:**
```c
void user_extract_float_array(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 4) return;
    if (!inst || !inst->blackboard) return;
    
    // Get dictionary pointer
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    // Get path
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    // Get base field offset and count
    uint16_t base_offset = params[2].field_offset;
    uint16_t count = (uint16_t)params[3].int_val;
    
    // Navigate to array
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    const s_expr_param_t* array = se_dicts_get_array(dict, mod_def, path);
    if (!array) return;
    
    // Extract elements into contiguous float fields
    uint8_t* bb = (uint8_t*)inst->blackboard;
    float* dest = (float*)(bb + base_offset);
    
    se_arrays_iter_t iter;
    se_arrays_iter_init(&iter, array);
    
    const s_expr_param_t* value;
    uint16_t index;
    
    while (se_arrays_iter_next(&iter, &value, &index) && index < count) {
        dest[index] = (float)se_dicts_param_float(value, 0.0f);
    }
}
```

**Usage:**
```lua
RECORD("sensor_data")
    PTR64_FIELD("config_ptr", "void")
    -- Contiguous array of floats
    FIELD("calibration", "float", 8)  -- 8 floats
END_RECORD()

se_sequence(function()
    se_load_dictionary("config_ptr", config)
    -- Extract up to 8 floats starting at calibration field
    se_dict_extract_float_array("config_ptr", "sensors.calibration", "calibration", 8)
end)
```

### Array of Structures Pattern

For arrays of dictionaries, a common pattern is to iterate and process each:

**Configuration:**
```lua
local config = {
    valves = {
        {id = 1, pin = 10, timeout = 5000},
        {id = 2, pin = 11, timeout = 3000},
        {id = 3, pin = 12, timeout = 4000},
    }
}
```

**Custom Handler:**
```c
typedef struct {
    uint32_t id;
    uint32_t pin;
    uint32_t timeout;
} valve_config_t;

void user_extract_valve_array(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 4) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    uint16_t base_offset = params[2].field_offset;
    uint16_t max_count = (uint16_t)params[3].int_val;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    const s_expr_param_t* array = se_dicts_get_array(dict, mod_def, path);
    if (!array) return;
    
    uint8_t* bb = (uint8_t*)inst->blackboard;
    valve_config_t* valves = (valve_config_t*)(bb + base_offset);
    
    se_arrays_iter_t iter;
    se_arrays_iter_init(&iter, array);
    
    const s_expr_param_t* elem;
    uint16_t index;
    
    while (se_arrays_iter_next(&iter, &elem, &index) && index < max_count) {
        if (!se_dicts_is_dict(elem)) continue;
        
        valves[index].id = (uint32_t)se_dicts_get_uint(elem, mod_def, "id", 0);
        valves[index].pin = (uint32_t)se_dicts_get_uint(elem, mod_def, "pin", 0);
        valves[index].timeout = (uint32_t)se_dicts_get_uint(elem, mod_def, "timeout", 0);
    }
}
```

### Array Functions Summary

| Function | Library | Description |
|----------|---------|-------------|
| `se_dicts_get_array()` | string | Get array param by path |
| `se_dicts_array_count()` | string | Count elements in array |
| `se_dicts_array_get()` | string | Get element by index |
| `se_arrays_iter_init()` | string | Initialize array iterator |
| `se_arrays_iter_next()` | string | Get next element |
| `se_arrays_iter_reset()` | string | Reset iterator to start |
| `se_dicth_get_array()` | hash | Get array param by hash path |
| `se_dicth_array_count()` | hash | Count elements in array |
| `se_dicth_array_get()` | hash | Get element by index |
| `se_arrayh_iter_init()` | hash | Initialize array iterator |
| `se_arrayh_iter_next()` | hash | Get next element |
| `se_arrayh_iter_reset()` | hash | Reset iterator to start |

