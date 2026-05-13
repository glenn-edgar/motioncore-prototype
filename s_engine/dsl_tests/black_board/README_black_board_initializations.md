# Blackboard Pre-Initialization

The tree instance contains a blackboard pointer that users can access to initialize fields **before** the first tick.

## Basic Pattern

```c
// Create tree
s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
    &engine->module,
    MY_TREE_HASH,
    0
);

// Get blackboard pointer
ScalarDemo_t* bb = (ScalarDemo_t*)s_expr_tree_get_blackboard(tree);

// Initialize fields before first tick
bb->counter = 100;
bb->flags = 0xABCD;
bb->temperature = 25.5f;

// Now tick - tree sees initialized values
s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
```

## External Blackboard Pattern

Alternatively, bind your own struct:

```c
// Application owns the data
ScalarDemo_t app_state = {
    .counter = 100,
    .flags = 0xABCD,
    .temperature = 25.5f,
    .timestamp = time(NULL),
    .checksum = 0,
    .precise_value = 3.14159
};

// Create tree
s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
    &engine->module,
    MY_TREE_HASH,
    0
);

// Bind external blackboard
s_expr_tree_bind_blackboard(tree, &app_state, sizeof(app_state));

// Tick - tree operates on app_state
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);

// Read results back
printf("Counter after tick: %d\n", app_state.counter);
```

## API Functions

| Function | Description |
|----------|-------------|
| `s_expr_tree_get_blackboard(tree)` | Get raw pointer to blackboard |
| `s_expr_tree_get_blackboard_size(tree)` | Get blackboard size in bytes |
| `s_expr_tree_bind_blackboard(tree, ptr, size)` | Bind external blackboard |

## Field Access by Hash

For dynamic field access without knowing the struct layout:

```c
// By hash (pre-computed)
s_expr_hash_t field_hash = s_expr_hash("counter");
int32_t* counter = s_expr_blackboard_get_field_by_hash(tree, field_hash);
*counter = 42;

// By string (computes hash internally)
int32_t* counter2 = s_expr_blackboard_get_field_by_string(tree, "counter");
*counter2 = 42;
```

## Typed Accessors

Convenience functions for common types:

```c
// Set fields
s_expr_blackboard_set_int_by_string(tree, "counter", 100);
s_expr_blackboard_set_float_by_string(tree, "temperature", 25.5f);
s_expr_blackboard_set_uint_by_string(tree, "flags", 0xABCD);

// Get fields (with default if not found)
int32_t counter = s_expr_blackboard_get_int_by_string(tree, "counter", 0);
float temp = s_expr_blackboard_get_float_by_string(tree, "temperature", 0.0f);
```

## Use Cases

### 1. Configuration Injection

```c
// Load config from file/network
Config_t config = load_config("settings.json");

// Create behavior tree
s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(...);

// Inject config into blackboard
BehaviorState_t* bb = s_expr_tree_get_blackboard(tree);
bb->max_speed = config.max_speed;
bb->timeout_ms = config.timeout;
bb->retry_count = config.retries;

// Run tree with configured values
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
```

### 2. Sensor Data Input

```c
// Read sensors
float temperature = read_temperature_sensor();
float humidity = read_humidity_sensor();
uint32_t pressure = read_pressure_sensor();

// Update blackboard
SensorState_t* bb = s_expr_tree_get_blackboard(tree);
bb->temperature = temperature;
bb->humidity = humidity;
bb->pressure = pressure;

// Tree processes sensor data
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);

// Read actuator outputs
if (bb->fan_enable) {
    enable_fan(bb->fan_speed);
}
```

### 3. Inter-Tick State Updates

```c
while (running) {
    // Update blackboard with external events
    ControlState_t* bb = s_expr_tree_get_blackboard(tree);
    
    if (button_pressed()) {
        bb->button_event = 1;
    }
    
    if (message_received(&msg)) {
        bb->message_id = msg.id;
        bb->message_data = msg.data;
    }
    
    // Tick tree
    s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    
    // Clear one-shot events
    bb->button_event = 0;
    
    if (result == SE_TERMINATE) {
        break;
    }
    
    sleep_ms(10);
}
```

### 4. Shared State Between Trees

```c
// Single blackboard shared by multiple trees
SharedState_t shared = {0};

// Bind to multiple trees
s_expr_tree_bind_blackboard(tree1, &shared, sizeof(shared));
s_expr_tree_bind_blackboard(tree2, &shared, sizeof(shared));
s_expr_tree_bind_blackboard(tree3, &shared, sizeof(shared));

// Initialize once
shared.system_time = get_system_time();
shared.global_flags = SYSTEM_READY;

// Each tree sees and can modify shared state
s_expr_node_tick(tree1, SE_EVENT_TICK, NULL);
s_expr_node_tick(tree2, SE_EVENT_TICK, NULL);
s_expr_node_tick(tree3, SE_EVENT_TICK, NULL);
```

## Engine-Allocated vs External Blackboard

| Aspect | Engine-Allocated | External |
|--------|------------------|----------|
| Allocation | Engine handles | User handles |
| `use_defaults()` | Applied automatically | User must initialize |
| Lifetime | Freed with tree | User controls |
| Sharing | One tree only | Multiple trees possible |
| Memory location | Heap (engine allocator) | User choice (stack, heap, static) |

## Timing

```
┌─────────────────────────────────────────────────────────┐
│                    Tree Lifecycle                        │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  s_expr_tree_create()                                   │
│       │                                                 │
│       ▼                                                 │
│  ┌─────────────────────────────────┐                   │
│  │  Blackboard accessible here     │ ◄── Initialize    │
│  │  bb = s_expr_tree_get_blackboard│     fields here   │
│  │  bb->field = value;             │                   │
│  └─────────────────────────────────┘                   │
│       │                                                 │
│       ▼                                                 │
│  s_expr_node_tick() ◄── Tree reads/writes blackboard   │
│       │                                                 │
│       ▼                                                 │
│  ┌─────────────────────────────────┐                   │
│  │  Blackboard accessible here     │ ◄── Read results  │
│  │  result = bb->output_field;     │     Update inputs │
│  └─────────────────────────────────┘                   │
│       │                                                 │
│       ▼                                                 │
│  s_expr_node_tick() ◄── Next tick                      │
│       │                                                 │
│      ...                                                │
│       │                                                 │
│       ▼                                                 │
│  s_expr_tree_free()                                     │
│                                                         │
└─────────────────────────────────────────────────────────┘
```