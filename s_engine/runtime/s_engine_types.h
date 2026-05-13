// ============================================================================
// s_engine_types.h
// S-Expression Engine Type Definitions
// Flat parameter model, hash-based function tables, record/field blackboards
// ============================================================================

#ifndef S_ENGINE_TYPES_H
#define S_ENGINE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 64-BIT CONFIGURATION
// ============================================================================
// Silence unused parameter warnings
#define UNUSED(x) (void)(x)


#ifndef MODULE_IS_64BIT
    #define MODULE_IS_64BIT 0
#endif

#ifndef CT_TYPES_DEFINED
    #if MODULE_IS_64BIT
        typedef int64_t  ct_int_t;
        typedef uint64_t ct_uint_t;
        typedef double   ct_float_t;
    #else
        typedef int32_t  ct_int_t;
        typedef uint32_t ct_uint_t;
        typedef float    ct_float_t;
    #endif
    #define CT_TYPES_DEFINED 1
#endif

// ============================================================================
// HASH TYPE (32-bit or 64-bit based on MODULE_IS_64BIT)
// ============================================================================

#if MODULE_IS_64BIT
    typedef uint64_t s_expr_hash_t;
    #define S_EXPR_HASH_FMT "0x%016llX"
    #define S_EXPR_HASH_SUFFIX ULL
    
    // FNV-1a 64-bit constants
    #define FNV_PRIME  0x00000100000001B3ULL
    #define FNV_OFFSET 0xCBF29CE484222325ULL
#else
    typedef uint32_t s_expr_hash_t;
    #define S_EXPR_HASH_FMT "0x%08X"
    #define S_EXPR_HASH_SUFFIX U
    
    // FNV-1a 32-bit constants
    #define FNV_PRIME  0x01000193U
    #define FNV_OFFSET 0x811C9DC5U
#endif


// Forward declaration (near top)
typedef struct s_expr_stack s_expr_stack_t;

// ============================================================================
// RESULT CODES (returned by main functions)
// ============================================================================

typedef enum {
    // APPLICATION RESULT CODES (0-5)
    SE_CONTINUE           = 0,
    SE_HALT               = 1,
    SE_TERMINATE          = 2,
    SE_RESET              = 3,
    SE_DISABLE            = 4,
    SE_SKIP_CONTINUE      = 5,
    
    // FUNCTION RESULT CODES (6-11)
    SE_FUNCTION_CONTINUE      = 6,
    SE_FUNCTION_HALT          = 7,
    SE_FUNCTION_TERMINATE     = 8,
    SE_FUNCTION_RESET         = 9,
    SE_FUNCTION_DISABLE       = 10,
    SE_FUNCTION_SKIP_CONTINUE = 11,
    
    // PIPELINE RESULT CODES (12-17)
    SE_PIPELINE_CONTINUE      = 12,
    SE_PIPELINE_HALT          = 13,
    SE_PIPELINE_TERMINATE     = 14,
    SE_PIPELINE_RESET         = 15,
    SE_PIPELINE_DISABLE       = 16,
    SE_PIPELINE_SKIP_CONTINUE = 17,
} s_expr_result_t;

// ============================================================================
// EVENT TYPES (passed to functions)
// ============================================================================

typedef enum {
    SE_EVENT_TICK      = 4,     /* matches CFL_TIMER_EVENT */
    SE_EVENT_INIT      = 0xfffe,
    SE_EVENT_TERMINATE = 0xfffd,

} s_expr_event_type_t;

// ============================================================================
// PARAMETER TYPE OPCODES (bits 5:0 of type byte)
// ============================================================================


#define S_EXPR_EVENT_QUEUE_SIZE 16

typedef struct {
    uint16_t tick_type;
    uint16_t event_id;
    void* event_data;
} s_expr_queued_event_t;

// Add these fields to s_expr_tree_instance_t:
//     s_expr_queued_event_t event_queue[S_EXPR_EVENT_QUEUE_SIZE];
//     uint8_t event_queue_head;
//     uint8_t event_queue_count;

// Primitive values
#define S_EXPR_PARAM_INT         0x00
#define S_EXPR_PARAM_UINT        0x01
#define S_EXPR_PARAM_FLOAT       0x02
#define S_EXPR_PARAM_STR_HASH    0x03

// Legacy/reserved
#define S_EXPR_PARAM_SLOT        0x04

// List structure (basic grouping)
#define S_EXPR_PARAM_OPEN        0x05   // list_start()
#define S_EXPR_PARAM_CLOSE       0x06   // list_end()

// Function call structure
#define S_EXPR_PARAM_OPEN_CALL   0x07   // start_call() - function invocation

// Function references
#define S_EXPR_PARAM_ONESHOT     0x08
#define S_EXPR_PARAM_MAIN        0x09
#define S_EXPR_PARAM_PRED        0x0A

// Field/record access
#define S_EXPR_PARAM_FIELD       0x0B   // Both simple and nested field refs
#define S_EXPR_PARAM_RESULT      0x0C
#define S_EXPR_PARAM_STR_IDX     0x0D   // String table index
#define S_EXPR_PARAM_CONST_REF   0x0E   // Constant reference (ROM)

// Reserved
#define S_EXPR_PARAM_RESERVED_0F 0x0F

// Dictionary/hash structure (key-value collections)
#define S_EXPR_PARAM_OPEN_DICT   0x10   // dict_start() - begin dictionary
#define S_EXPR_PARAM_CLOSE_DICT  0x11   // dict_end()   - end dictionary
#define S_EXPR_PARAM_OPEN_KEY    0x12   // key_start()  - begin key-value pair
#define S_EXPR_PARAM_CLOSE_KEY   0x13   // key_end()    - end key-value pair

// Array structure (indexed collections)
#define S_EXPR_PARAM_OPEN_ARRAY  0x14   // array_start() - begin array
#define S_EXPR_PARAM_CLOSE_ARRAY 0x15   // array_end()   - end array

// Tuple structure (fixed-size heterogeneous)
#define S_EXPR_PARAM_OPEN_TUPLE  0x16   // tuple_start() - begin tuple
#define S_EXPR_PARAM_CLOSE_TUPLE 0x17   // tuple_end()   - end tuple

// Stack operation opcodes
#define S_EXPR_PARAM_STACK_TOS   0x18   // stack_tos(offset)
#define S_EXPR_PARAM_STACK_LOCAL 0x19   // stack_local(index)
#define S_EXPR_PARAM_NULL        0x1A   // null_param()
#define S_EXPR_PARAM_STACK_PUSH  0x1B   // stack_push(value)
#define S_EXPR_PARAM_STACK_POP   0x1C   // stack_pop()

// ============================================================================
// TYPE FLAGS (upper bits of type byte)
// ============================================================================

#define S_EXPR_FLAG_SURVIVES_RESET 0x40  // bit 6: io_call (survives reset)
#define S_EXPR_FLAG_POINTER        0x80  // bit 7: pt_m_call (pointer-capable)
#define S_EXPR_OPCODE_MASK         0x3F  // bits 5:0 (allows 64 opcodes)

// ============================================================================
// PARAMETER TYPE PREDICATES
// ============================================================================

#define S_EXPR_PARAM_IS_NUMERIC(t)   (((t) & S_EXPR_OPCODE_MASK) <= S_EXPR_PARAM_FLOAT)
#define S_EXPR_PARAM_IS_FUNC_REF(t)  (((t) & S_EXPR_OPCODE_MASK) >= S_EXPR_PARAM_ONESHOT && \
                                      ((t) & S_EXPR_OPCODE_MASK) <= S_EXPR_PARAM_PRED)

// Basic list braces
#define S_EXPR_PARAM_IS_OPEN(t)      (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN || \
                                      ((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_CALL)
#define S_EXPR_PARAM_IS_CLOSE(t)     (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE)
#define S_EXPR_PARAM_IS_CALLABLE(t)  (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_CALL)

// Dictionary predicates
#define S_EXPR_PARAM_IS_OPEN_DICT(t)  (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_DICT)
#define S_EXPR_PARAM_IS_CLOSE_DICT(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_DICT)
#define S_EXPR_PARAM_IS_OPEN_KEY(t)   (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_KEY)
#define S_EXPR_PARAM_IS_CLOSE_KEY(t)  (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_KEY)
#define S_EXPR_PARAM_IS_DICT(t)       (S_EXPR_PARAM_IS_OPEN_DICT(t) || S_EXPR_PARAM_IS_CLOSE_DICT(t))
#define S_EXPR_PARAM_IS_KEY(t)        (S_EXPR_PARAM_IS_OPEN_KEY(t) || S_EXPR_PARAM_IS_CLOSE_KEY(t))

// Array predicates
#define S_EXPR_PARAM_IS_OPEN_ARRAY(t)  (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_ARRAY)
#define S_EXPR_PARAM_IS_CLOSE_ARRAY(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_ARRAY)
#define S_EXPR_PARAM_IS_ARRAY(t)       (S_EXPR_PARAM_IS_OPEN_ARRAY(t) || S_EXPR_PARAM_IS_CLOSE_ARRAY(t))

// Tuple predicates
#define S_EXPR_PARAM_IS_OPEN_TUPLE(t)  (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_TUPLE)
#define S_EXPR_PARAM_IS_CLOSE_TUPLE(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_TUPLE)
#define S_EXPR_PARAM_IS_TUPLE(t)       (S_EXPR_PARAM_IS_OPEN_TUPLE(t) || S_EXPR_PARAM_IS_CLOSE_TUPLE(t))

// Any structured open/close (for generic iteration)
#define S_EXPR_PARAM_IS_ANY_OPEN(t)  (S_EXPR_PARAM_IS_OPEN(t) || \
                                      S_EXPR_PARAM_IS_OPEN_DICT(t) || \
                                      S_EXPR_PARAM_IS_OPEN_KEY(t) || \
                                      S_EXPR_PARAM_IS_OPEN_ARRAY(t) || \
                                      S_EXPR_PARAM_IS_OPEN_TUPLE(t))
#define S_EXPR_PARAM_IS_ANY_CLOSE(t) (S_EXPR_PARAM_IS_CLOSE(t) || \
                                      S_EXPR_PARAM_IS_CLOSE_DICT(t) || \
                                      S_EXPR_PARAM_IS_CLOSE_KEY(t) || \
                                      S_EXPR_PARAM_IS_CLOSE_ARRAY(t) || \
                                      S_EXPR_PARAM_IS_CLOSE_TUPLE(t))

// Field/slot/string predicates
#define S_EXPR_PARAM_IS_SLOT(t)      (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_SLOT)
#define S_EXPR_PARAM_IS_FIELD(t)     (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_FIELD)
#define S_EXPR_PARAM_IS_RESULT(t)    (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_RESULT)
#define S_EXPR_PARAM_HAS_POINTER(t)  (((t) & S_EXPR_FLAG_POINTER) != 0)
#define S_EXPR_PARAM_SURVIVES_RESET(t) (((t) & S_EXPR_FLAG_SURVIVES_RESET) != 0)
#define S_EXPR_PARAM_IS_STR_IDX(t)   (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_STR_IDX)
#define S_EXPR_PARAM_IS_CONST_REF(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CONST_REF)
#define S_EXPR_PARAM_IS_STACK_TOS(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_STACK_TOS)
#define S_EXPR_PARAM_IS_STACK_LOCAL(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_STACK_LOCAL)
#define S_EXPR_PARAM_IS_NULL(t) (((t) & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_NULL)
// ============================================================================
// NODE FLAGS (runtime state per func_node)
// ============================================================================

#define S_EXPR_NODE_FLAG_ACTIVE        0x01
#define S_EXPR_NODE_FLAG_INITIALIZED   0x02
#define S_EXPR_NODE_FLAG_EVER_INIT     0x04
#define S_EXPR_NODE_FLAG_ERROR         0x08
#define S_EXPR_NODE_FLAGS_SYSTEM       0x0F
#define S_EXPR_NODE_FLAGS_USER         0xF0

// ============================================================================
// PARAMETER STRUCTURE (matches DSL output)
//
// DSL output examples:
//   { .type = 0x09, .index_to_pointer = 0, .node_index = 1, .func_index = 4 }
//   { .type = 0x0B, .field_offset = 0, .field_size = 4 }
//   { .type = 0x00, .int_val = 1 }
//   { .type = 0x07, .brace_idx = 4 }
//   { .type = 0x10, .brace_idx = 8 }   // OPEN_DICT
//   { .type = 0x12, .str_hash = 0x... } // OPEN_KEY with key hash
//
// Fields:
//   node_index       -> index into node_states[] (flags/state)
//   func_index       -> index into function table
//   index_to_pointer -> index into pointer_array[] (for pt_m_call)
// ============================================================================

typedef struct {
    uint8_t  type;              // opcode + flags
    uint8_t  index_to_pointer;  // pointer array index (for pt_m_call)
    union {
        // Stack references (STACK_TOS, STACK_LOCAL)
        struct {
            uint16_t stack_offset;
            uint16_t stack_reserved;
        };
        // Constant references (CONST_REF)
        struct {
            uint16_t const_index;
            uint16_t const_size;
        };
        // Function references (ONESHOT, MAIN, PRED)
        struct {
            uint16_t node_index;
            uint16_t func_index;
        };
        // Field references (FIELD)
        struct {
            uint16_t field_offset;
            uint16_t field_size;
        };
        // Slot references (SLOT) - legacy
        struct {
            uint16_t pool_id;
            uint16_t slot_index;
        };
        // String table reference (STR_IDX)
        struct {
            uint16_t str_index;
            uint16_t str_len;
        };
        // Brace matching + parent navigation
        // (OPEN, OPEN_CALL, CLOSE, OPEN_DICT, CLOSE_DICT,
        //  OPEN_ARRAY, CLOSE_ARRAY, OPEN_TUPLE, CLOSE_TUPLE, CLOSE_KEY)
        struct {
            uint16_t brace_idx;       // forward/backward skip to matching brace
            uint16_t parent_offset;   // relative offset back to enclosing OPEN_CALL
                                      // 0 = top-level / no parent
        };
        // Values (INT, UINT, FLOAT, STR_HASH, RESULT)
        ct_int_t      int_val;
        ct_uint_t     uint_val;
        ct_float_t    float_val;
        s_expr_hash_t str_hash;     // Also used for OPEN_KEY key hash
    };
} s_expr_param_t;

// Verify sizes at compile time
#if MODULE_IS_64BIT
    _Static_assert(sizeof(s_expr_param_t) == 16, "64-bit param should be 16 bytes");
#else
    _Static_assert(sizeof(s_expr_param_t) == 8, "32-bit param should be 8 bytes");
#endif

// ============================================================================
// NODE STATE (RAM - per func_node in tree instance)
// Minimal structure - pointer/u64/f64 storage is in separate pointer_array
// ============================================================================

typedef struct {
    uint8_t  flags;         // system (0-3) + user (4-7) flags
    uint8_t  state;         // user state (0-255)
    uint16_t user_data;     // 16 bits for dispatch tracking, etc.
} s_expr_node_state_t;

_Static_assert(sizeof(s_expr_node_state_t) == 4, "node state should be 4 bytes");

// ============================================================================
// POINTER SLOT (always 8 bytes for ptr/u64/i64/f64 storage)
// Used by pt_m_call functions for persistent storage
// ============================================================================

typedef union {
    void*    ptr;
    uint64_t u64;
    int64_t  i64;
    double   f64;
} s_expr_slot_t;

#define S_EXPR_SLOT_FLAG_NONE       0x00
#define S_EXPR_SLOT_FLAG_ALLOCATED  0x01  // engine allocated - freeable
#define S_EXPR_SLOT_FLAG_EXTERNAL   0x02  // user provided - do not free

_Static_assert(sizeof(s_expr_slot_t) == 8, "slot should be 8 bytes");

// ============================================================================
// FIELD DESCRIPTOR (for runtime field lookup)
// ============================================================================

typedef struct {
    s_expr_hash_t name_hash;
    uint16_t offset;
    uint16_t size;
} s_expr_field_desc_t;

// ============================================================================
// RECORD DESCRIPTOR (for runtime record management)
// ============================================================================

typedef struct {
    s_expr_hash_t name_hash;
    uint16_t total_size;
    uint16_t field_count;
    const s_expr_field_desc_t* fields;
} s_expr_record_desc_t;

// ============================================================================
// TREE DEFINITION (ROM - generated by DSL)
// ============================================================================

typedef struct {
    s_expr_hash_t name_hash;
    s_expr_hash_t record_hash;
    const s_expr_param_t* params;
    uint16_t param_count;
    uint16_t func_node_count;
    uint16_t pointer_count;
    uint16_t defaults_index;    // NEW: 0xFFFF = no defaults
} s_expr_tree_def_t;

#define S_EXPR_NO_DEFAULTS 0xFFFF
// ============================================================================
// MODULE DEFINITION (ROM - generated by DSL)
// ============================================================================

typedef struct {
    s_expr_hash_t name_hash;
    const s_expr_tree_def_t* trees;
    uint16_t tree_count;
    bool     is_64bit;
    
    const s_expr_hash_t* oneshot_hashes;
    const s_expr_hash_t* main_hashes;
    const s_expr_hash_t* pred_hashes;
    
    uint16_t oneshot_count;
    uint16_t main_count;
    uint16_t pred_count;
    
    uint16_t max_func_node_count;
    uint16_t max_pointer_count;
    uint16_t max_param_count;
    
    const s_expr_record_desc_t* records;
    uint16_t record_count;
    
    // String table (interned strings)
    const char* const* string_table;
    uint16_t string_count;
    const void* const* constants;  // array of pointers to const structs
    uint16_t const_count;
} s_expr_module_def_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

typedef struct s_expr_module s_expr_module_t;
typedef struct s_expr_tree_instance s_expr_tree_instance_t;
typedef struct s_expr_stack s_expr_stack_t;

// ============================================================================
// FUNCTION SIGNATURES
// ============================================================================

typedef s_expr_result_t (*s_expr_main_fn_t)(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
);

typedef void (*s_expr_oneshot_fn_t)(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
);

typedef bool (*s_expr_pred_fn_t)(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
);

typedef void (*s_expr_debug_fn_t)(
    s_expr_tree_instance_t* inst,
    const char* message
);

typedef void (*s_expr_error_fn_t)(
    s_expr_tree_instance_t* inst,
    uint8_t error_code,
    const char* message
);

// ============================================================================
// MEMORY ALLOCATOR INTERFACE
// ============================================================================

typedef void*    (*s_expr_malloc_fn_t) (void* ctx, size_t size);
typedef void     (*s_expr_free_fn_t)   (void* ctx, void* ptr);
typedef double   (*s_expr_time_fn_t)   (void* ctx);
typedef uint32_t (*s_expr_time_ms_fn_t)(void* ctx);

typedef struct {
    s_expr_malloc_fn_t   malloc;
    s_expr_free_fn_t     free;
    void*                ctx;
    s_expr_time_fn_t     get_time;     // monotonic time in seconds (double)
    // Optional: monotonic uptime in milliseconds (uint32_t). When provided,
    // se_log* and s_engine_log use it instead of multiplying get_time by
    // 1000.0 — avoids pulling in newlib's __aeabi_dmul (~1.2 KB) and
    // __aeabi_ddiv (~1.5 KB) on M-port targets that don't otherwise need
    // double-precision floating-point math. Leave NULL if get_time is enough.
    s_expr_time_ms_fn_t  get_time_ms;
} s_expr_allocator_t;

// ============================================================================
// FUNCTION REGISTRATION (hash -> function pointer)
// ============================================================================

typedef struct {
    const char* name;
    void*       fn_ptr;
} s_expr_fn_entry_named_t;

typedef struct {
    s_expr_hash_t hash;
    void*         fn_ptr;
} s_expr_fn_entry_t;

typedef struct {
    const s_expr_fn_entry_t* entries;
    uint16_t                 count;
} s_expr_fn_table_t;

// ============================================================================
// SHARED MODULE (created once per module definition)
// ============================================================================

struct s_expr_module {
    const s_expr_module_def_t* def;
    
    s_expr_oneshot_fn_t* oneshot_fns;
    s_expr_main_fn_t*    main_fns;
    s_expr_pred_fn_t*    pred_fns;
    
    s_expr_debug_fn_t    debug_fn;
    s_expr_error_fn_t    error_fn;
    
    s_expr_allocator_t   alloc;
    
    void**               pool_table;
    uint16_t             pool_count;
    
    uint8_t              error_code;
    uint16_t             error_index;
    s_expr_hash_t        error_hash;
};

// ============================================================================
// TREE INSTANCE (created per-execution)
// ============================================================================

struct s_expr_tree_instance {
    s_expr_module_t*          module;
    const s_expr_tree_def_t*  tree;
    uint16_t                  tree_index;
    
    // Per func_node state (flags/state/user_data)
    s_expr_node_state_t*      node_states;
    uint16_t                  node_count;
    
    // Pointer/value array (for pt_m_call - stores ptr, u64, i64, or f64)
    s_expr_slot_t*            pointer_array;
    uint8_t*                  slot_flags;      // parallel array - ownership tracking
    uint16_t                  pointer_count;
    
    // Blackboard (record binding)
    void*                     blackboard;
    uint16_t                  blackboard_size;
    bool                      blackboard_owned;
    
    // Current execution context
    uint16_t                  current_node_index;
    uint16_t                  current_event_id;
    void*                     current_event_data;
    bool                      in_pointer_call;
    uint8_t                   pointer_base;
    
    // ChainTree node identifier
    uint32_t                  ct_node_id;
    
    // User context
    void*                     user_ctx;    
    void*                     current_dict;
    // Parameter stack (created via s_expr_tree_create_stack)
    s_expr_stack_t*           stack;
    
    // Event queue (created via s_expr_tree_create_event_queue)
    s_expr_queued_event_t    event_queue[S_EXPR_EVENT_QUEUE_SIZE];
    uint8_t                   event_queue_head;
    uint8_t                   event_queue_count;
    uint16_t                  tick_type;
};

// ============================================================================
// ERROR CODES
// ============================================================================

#define S_EXPR_ERR_OK                    0
#define S_EXPR_ERR_ALLOC                 1
#define S_EXPR_ERR_NULL_DEF              2
#define S_EXPR_ERR_64BIT_MISMATCH        3
#define S_EXPR_ERR_ONESHOT_NOT_FOUND     4
#define S_EXPR_ERR_MAIN_NOT_FOUND        5
#define S_EXPR_ERR_PRED_NOT_FOUND        6
#define S_EXPR_ERR_INVALID_TREE          7
#define S_EXPR_ERR_NOT_POINTER_CALL      8
#define S_EXPR_ERR_POINTER_INDEX         9
#define S_EXPR_ERR_NO_BLACKBOARD         10
#define S_EXPR_ERR_DICT_KEY_NOT_FOUND    11
#define S_EXPR_ERR_INVALID_STRUCTURE     12

// ============================================================================
// HASH FUNCTION (FNV-1a)
// ============================================================================

static inline s_expr_hash_t s_expr_hash(const char* str) {
    s_expr_hash_t hash = FNV_OFFSET;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= FNV_PRIME;
    }
    return hash;
}

// Alias for consistency
static inline s_expr_hash_t s_expr_fnv1a_hash(const char* str) {
    return s_expr_hash(str);
}

// ============================================================================
// BLACKBOARD FIELD ACCESS MACRO
// ============================================================================

#define S_EXPR_GET_FIELD(inst, param, type) \
    ((type*)((uint8_t*)(inst)->blackboard + (param)->field_offset))

// ============================================================================
// PARAMETER ACCESSORS
// ============================================================================

static inline ct_int_t s_expr_param_int(const s_expr_param_t* p) {
    return p->int_val;
}

static inline ct_uint_t s_expr_param_uint(const s_expr_param_t* p) {
    return p->uint_val;
}

static inline ct_float_t s_expr_param_float(const s_expr_param_t* p) {
    return p->float_val;
}

static inline s_expr_hash_t s_expr_param_str_hash(const s_expr_param_t* p) {
    return p->str_hash;
}

static inline uint8_t s_expr_param_opcode(const s_expr_param_t* p) {
    return p->type & S_EXPR_OPCODE_MASK;
}

static inline uint8_t s_expr_param_ptr_index(const s_expr_param_t* p) {
    return p->index_to_pointer;
}

// ============================================================================
// PARAMETER TYPE HELPERS
// ============================================================================

static inline bool s_expr_param_is_predicate(const s_expr_param_t* param) {
    uint8_t opcode = param->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_PRED) return true;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        return ((param + 1)->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_PRED;
    }
    return false;
}

static inline bool s_expr_param_is_oneshot(const s_expr_param_t* param) {
    uint8_t opcode = param->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_ONESHOT) return true;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        return ((param + 1)->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_ONESHOT;
    }
    return false;
}

static inline bool s_expr_param_is_main(const s_expr_param_t* param) {
    uint8_t opcode = param->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_MAIN) return true;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        return ((param + 1)->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_MAIN;
    }
    return false;
}

static inline bool s_expr_param_is_action(const s_expr_param_t* param) {
    uint8_t opcode = param->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_MAIN || opcode == S_EXPR_PARAM_ONESHOT) return true;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        uint8_t func_opcode = (param + 1)->type & S_EXPR_OPCODE_MASK;
        return func_opcode == S_EXPR_PARAM_MAIN || func_opcode == S_EXPR_PARAM_ONESHOT;
    }
    return false;
}

// ============================================================================
// RESULT CODE FINDER
// ============================================================================

static inline s_expr_result_t s_expr_find_result(
    const s_expr_param_t* params,
    uint16_t param_count
) {
    for (int16_t i = param_count - 1; i >= 0; i--) {
        if ((params[i].type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_RESULT) {
            return (s_expr_result_t)params[i].int_val;
        }
    }
    for (int16_t i = param_count - 1; i >= 0; i--) {
        uint8_t opcode = params[i].type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
            return (s_expr_result_t)params[i].int_val;
        }
    }
    return SE_CONTINUE;
}

// ============================================================================
// STRING TABLE ACCESS
// ============================================================================

static inline const char* s_expr_param_string(
    const s_expr_module_def_t* def,
    const s_expr_param_t* p
) {
    if (!def || !def->string_table) return NULL;
    if ((p->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_STR_IDX) return NULL;
    if (p->str_index >= def->string_count) return NULL;
    return def->string_table[p->str_index];
}

static inline const s_expr_module_def_t* s_expr_tree_get_module_def(
    s_expr_tree_instance_t* inst
) {
    if (!inst || !inst->module) return NULL;
    return inst->module->def;
}

// ============================================================================
// DICTIONARY/COLLECTION NAVIGATION HELPERS
// ============================================================================

// Skip to end of current structure (handles nested structures)
// Returns pointer to matching CLOSE token, or NULL on error
static inline const s_expr_param_t* s_expr_skip_structure(
    const s_expr_param_t* open_param,
    const s_expr_param_t* params_end
) {
    if (!open_param || open_param >= params_end) return NULL;
    
    uint8_t opcode = open_param->type & S_EXPR_OPCODE_MASK;
    uint8_t close_opcode;
    
    // Determine matching close opcode
    switch (opcode) {
        case S_EXPR_PARAM_OPEN:
        case S_EXPR_PARAM_OPEN_CALL:
            close_opcode = S_EXPR_PARAM_CLOSE;
            break;
        case S_EXPR_PARAM_OPEN_DICT:
            close_opcode = S_EXPR_PARAM_CLOSE_DICT;
            break;
        case S_EXPR_PARAM_OPEN_KEY:
            close_opcode = S_EXPR_PARAM_CLOSE_KEY;
            break;
        case S_EXPR_PARAM_OPEN_ARRAY:
            close_opcode = S_EXPR_PARAM_CLOSE_ARRAY;
            break;
        case S_EXPR_PARAM_OPEN_TUPLE:
            close_opcode = S_EXPR_PARAM_CLOSE_TUPLE;
            break;
        default:
            return NULL;  // Not an open token
    }
    
    // Use brace_idx for O(1) navigation
    uint16_t close_idx = open_param->brace_idx;
    const s_expr_param_t* close_param = open_param + close_idx;
    
    if (close_param < params_end && 
        (close_param->type & S_EXPR_OPCODE_MASK) == close_opcode) {
        return close_param;
    }
    
    return NULL;
}

// Get array element by index (0-based)
static inline const s_expr_param_t* s_expr_array_get(
    const s_expr_param_t* array_param,
    const s_expr_param_t* params_end,
    uint16_t index
) {
    if (!array_param || array_param >= params_end) return NULL;
    if ((array_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_OPEN_ARRAY) return NULL;
    
    const s_expr_param_t* p = array_param + 1;
    const s_expr_param_t* array_end = s_expr_skip_structure(array_param, params_end);
    if (!array_end) return NULL;
    
    uint16_t current = 0;
    while (p < array_end) {
        if (current == index) {
            return p;
        }
        
        // Skip nested structures
        if (S_EXPR_PARAM_IS_ANY_OPEN(p->type)) {
            p = s_expr_skip_structure(p, params_end);
            if (!p) return NULL;
        }
        p++;
        current++;
    }
    
    return NULL;
}

// ============================================================================
// NODE STATE ACCESSORS (for use in MAIN functions)
// ============================================================================

static inline uint8_t s_expr_get_state(s_expr_tree_instance_t* inst) {
    if (!inst || inst->current_node_index >= inst->node_count) return 0;
    return inst->node_states[inst->current_node_index].state;
}

static inline void s_expr_set_state(s_expr_tree_instance_t* inst, uint8_t state) {
    if (!inst || inst->current_node_index >= inst->node_count) return;
    inst->node_states[inst->current_node_index].state = state;
}

static inline uint16_t s_expr_get_user_flags(s_expr_tree_instance_t* inst) {
    if (!inst || inst->current_node_index >= inst->node_count) return 0;
    return inst->node_states[inst->current_node_index].user_data;
}

static inline void s_expr_set_user_flags(s_expr_tree_instance_t* inst, uint16_t flags) {
    if (!inst || inst->current_node_index >= inst->node_count) return;
    inst->node_states[inst->current_node_index].user_data = flags;
}

static inline uint8_t s_expr_get_system_flags(s_expr_tree_instance_t* inst) {
    if (!inst || inst->current_node_index >= inst->node_count) return 0;
    return inst->node_states[inst->current_node_index].flags;
}

static inline void s_expr_set_system_flags(s_expr_tree_instance_t* inst, uint8_t flags) {
    if (!inst || inst->current_node_index >= inst->node_count) return;
    inst->node_states[inst->current_node_index].flags = flags;
}
#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_TYPES_H