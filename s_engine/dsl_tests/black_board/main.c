#define _GNU_SOURCE
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_loader.h"
#include "s_engine_init.h"
#include "s_engine_builtins.h"
#include "s_engine_node.h"

#include "black_board.h"
#include "black_board_bin_32.h"

#include "black_board_records.h"

// ============================================================================
// SIMPLE ALLOCATOR
// ============================================================================

static void* simple_malloc(void* ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void simple_free(void* ctx, void* ptr) {
    (void)ctx;
    free(ptr);
}

// ============================================================================
// DEBUG/ERROR CALLBACKS
// ============================================================================

static int g_test_errors = 0;

static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst;
    printf("  [DEBUG] %s\n", msg);
}

static void error_callback(s_expr_tree_instance_t* inst, uint8_t error_code, const char* msg) {
    (void)inst;
    printf("  [ERROR %d] %s\n", error_code, msg);
    g_test_errors++;
}

// ============================================================================
// RESULT HELPERS
// ============================================================================

static const char* result_to_str(s_expr_result_t r) {
    switch (r) {
        // Application (0-5)
        case SE_CONTINUE:                return "CONTINUE";
        case SE_HALT:                    return "HALT";
        case SE_TERMINATE:               return "TERMINATE";
        case SE_RESET:                   return "RESET";
        case SE_DISABLE:                 return "DISABLE";
        case SE_SKIP_CONTINUE:           return "SKIP_CONTINUE";
        // Function (6-11)
        case SE_FUNCTION_CONTINUE:       return "FUNCTION_CONTINUE";
        case SE_FUNCTION_HALT:           return "FUNCTION_HALT";
        case SE_FUNCTION_TERMINATE:      return "FUNCTION_TERMINATE";
        case SE_FUNCTION_RESET:          return "FUNCTION_RESET";
        case SE_FUNCTION_DISABLE:        return "FUNCTION_DISABLE";
        case SE_FUNCTION_SKIP_CONTINUE:  return "FUNCTION_SKIP_CONTINUE";
        // Pipeline (12-17)
        case SE_PIPELINE_CONTINUE:       return "PIPELINE_CONTINUE";
        case SE_PIPELINE_HALT:           return "PIPELINE_HALT";
        case SE_PIPELINE_TERMINATE:      return "PIPELINE_TERMINATE";
        case SE_PIPELINE_RESET:          return "PIPELINE_RESET";
        case SE_PIPELINE_DISABLE:        return "PIPELINE_DISABLE";
        case SE_PIPELINE_SKIP_CONTINUE:  return "PIPELINE_SKIP_CONTINUE";
        default:                         return "UNKNOWN";
    }
}

// Linux monotonic time
static double linux_get_time(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

extern void black_board_register_all(s_expr_module_t* module);

// ============================================================================
// USER FUNCTION REGISTRATION WRAPPER
// ============================================================================

static void register_black_board_functions(s_engine_handle_t* engine) {
    black_board_register_all(&engine->module);
}

// ============================================================================
// TEST FUNCTIONS
// ============================================================================

static bool run_tree_test(s_engine_handle_t* engine, s_expr_hash_t tree_hash, 
                          const char* test_name, void* blackboard, uint16_t bb_size) {
    printf("\nTesting %s...\n", test_name);
    
    g_test_errors = 0;
    
    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &engine->module,
        tree_hash,
        0
    );
    
    if (!tree) {
        printf("  ❌ FAILED: Could not create tree (hash=0x%08X)\n", tree_hash);
        return false;
    }
    
    // Bind blackboard if provided
    if (blackboard && bb_size > 0) {
        s_expr_tree_bind_blackboard(tree, blackboard, bb_size);
    }
    
    // Run single tick (all oneshots execute)
    s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    
    printf("  Tree result: %s\n", result_to_str(result));
    
    if (g_test_errors > 0) {
        printf("  ❌ FAILED: %d verification errors\n", g_test_errors);
        s_expr_tree_free(tree);
        return false;
    }
    
    printf("  ✅ PASSED\n");
    s_expr_tree_free(tree);
    return true;
}

// ============================================================================
// BLACKBOARD ACCESS TESTS
// ============================================================================

static void test_blackboard_access(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    BLACKBOARD ACCESS TESTS             ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    // Let engine allocate blackboard with defaults
    run_tree_test(engine, DEMO_BLACKBOARD_ACCESS_HASH, 
                  "demo_blackboard_access", NULL, 0);
}

// ============================================================================
// SLOT ACCESS TESTS
// ============================================================================

static void test_slot_access(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    SLOT ACCESS TESTS                   ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    // Let engine allocate blackboard with defaults
    run_tree_test(engine, DEMO_SLOT_ACCESS_HASH,
                  "demo_slot_access", NULL, 0);
}

// ============================================================================
// ARRAY ACCESS TESTS
// ============================================================================

static void test_array_access(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    ARRAY ACCESS TESTS                  ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    // Let engine allocate blackboard with defaults
    run_tree_test(engine, DEMO_ARRAY_ACCESS_HASH,
                  "demo_array_access", NULL, 0);
}

// ============================================================================
// NESTED RECORD ACCESS TESTS
// ============================================================================

static void test_nested_access(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    NESTED RECORD ACCESS TESTS          ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    // Let engine allocate blackboard with defaults
    run_tree_test(engine, DEMO_NESTED_ACCESS_HASH,
                  "demo_nested_access", NULL, 0);
}

// ============================================================================
// CONSTANT INITIALIZATION TESTS
// ============================================================================

static void test_constants(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    CONSTANT INITIALIZATION TESTS       ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    // Let engine allocate blackboard - use_defaults() will initialize it
    // Pass NULL blackboard so engine uses its own allocation with defaults
    run_tree_test(engine, DEMO_CONSTANTS_HASH,
                  "demo_constants", NULL, 0);
}

// ============================================================================
// EXTERNAL BLACKBOARD TEST
// Demonstrates binding a pre-existing application struct to a tree
// ============================================================================

static void test_external_blackboard(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    EXTERNAL BLACKBOARD TEST            ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    printf("\nTesting external blackboard binding...\n");
    
    g_test_errors = 0;
    
    // Simulate application-owned data structure
    // This could be sensor data, hardware state, config, etc.
    ScalarDemo_t app_state = {
        .counter = 999,
        .flags = 0xABCD1234,
        .temperature = 25.5f,
        .timestamp = 1234567890LL,
        .checksum = 0xDEADBEEFCAFEBABEULL,
        .precise_value = 3.14159265358979
    };
    
    printf("  Initial app_state:\n");
    printf("    counter:       %d\n", app_state.counter);
    printf("    flags:         0x%08X\n", app_state.flags);
    printf("    temperature:   %.2f\n", app_state.temperature);
    printf("    timestamp:     %lld\n", (long long)app_state.timestamp);
    printf("    checksum:      0x%016llX\n", (unsigned long long)app_state.checksum);
    printf("    precise_value: %.15f\n", app_state.precise_value);
    
    // Create tree
    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &engine->module,
        DEMO_SLOT_ACCESS_HASH,  // Reuse slot_access tree
        0
    );
    
    if (!tree) {
        printf("  ❌ FAILED: Could not create tree\n");
        return;
    }
    
    // Bind external blackboard - tree operates directly on app_state
    s_expr_tree_bind_blackboard(tree, &app_state, sizeof(app_state));
    
    // Run tree - it will modify app_state in place
    s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("\n  Tree result: %s\n", result_to_str(result));
    
    // Show that app_state was modified by the tree
    printf("\n  Final app_state (modified by tree):\n");
    printf("    counter:       %d\n", app_state.counter);
    printf("    flags:         0x%08X\n", app_state.flags);
    printf("    temperature:   %.2f\n", app_state.temperature);
    printf("    timestamp:     %lld\n", (long long)app_state.timestamp);
    printf("    checksum:      0x%016llX\n", (unsigned long long)app_state.checksum);
    printf("    precise_value: %.15f\n", app_state.precise_value);
    
    if (g_test_errors > 0) {
        printf("  ❌ FAILED: %d verification errors\n", g_test_errors);
    } else {
        printf("  ✅ PASSED\n");
    }
    
    s_expr_tree_free(tree);
    
    // Demonstrate: app_state persists after tree is freed
    printf("\n  app_state persists after tree freed:\n");
    printf("    counter: %d (still accessible)\n", app_state.counter);
}

// ============================================================================
// SHARED BLACKBOARD TEST
// Demonstrates multiple trees sharing the same blackboard
// ============================================================================

static void test_shared_blackboard(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    SHARED BLACKBOARD TEST              ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    printf("\nTesting shared blackboard between trees...\n");
    
    g_test_errors = 0;
    
    // Single blackboard shared by multiple trees
    ScalarDemo_t shared_data;
    memset(&shared_data, 0, sizeof(shared_data));
    
    printf("  Initial shared_data: counter=%d\n", shared_data.counter);
    
    // Create first tree
    s_expr_tree_instance_t* tree1 = s_expr_tree_create_by_hash(
        &engine->module,
        DEMO_BLACKBOARD_ACCESS_HASH,
        1  // ct_node_id = 1
    );
    
    // Create second tree
    s_expr_tree_instance_t* tree2 = s_expr_tree_create_by_hash(
        &engine->module,
        DEMO_SLOT_ACCESS_HASH,
        2  // ct_node_id = 2
    );
    
    if (!tree1 || !tree2) {
        printf("  ❌ FAILED: Could not create trees\n");
        if (tree1) s_expr_tree_free(tree1);
        if (tree2) s_expr_tree_free(tree2);
        return;
    }
    
    // Both trees share the same blackboard
    s_expr_tree_bind_blackboard(tree1, &shared_data, sizeof(shared_data));
    s_expr_tree_bind_blackboard(tree2, &shared_data, sizeof(shared_data));
    
    // Run tree1 - modifies shared_data
    printf("\n  Running tree1 (blackboard_access)...\n");
    s_expr_result_t result1 = s_expr_node_tick(tree1, SE_EVENT_TICK, NULL);
    printf("    result: %s\n", result_to_str(result1));
    printf("    shared_data.counter after tree1: %d\n", shared_data.counter);
    printf("    shared_data.flags after tree1:   0x%08X\n", shared_data.flags);
    
    // Run tree2 - sees tree1's changes, makes its own
    printf("\n  Running tree2 (slot_access)...\n");
    s_expr_result_t result2 = s_expr_node_tick(tree2, SE_EVENT_TICK, NULL);
    printf("    result: %s\n", result_to_str(result2));
    printf("    shared_data.counter after tree2: %d\n", shared_data.counter);
    printf("    shared_data.flags after tree2:   0x%08X\n", shared_data.flags);
    
    if (g_test_errors > 0) {
        printf("\n  ❌ FAILED: %d verification errors\n", g_test_errors);
    } else {
        printf("\n  ✅ PASSED - Both trees operated on shared data\n");
    }
    
    s_expr_tree_free(tree1);
    s_expr_tree_free(tree2);
}

// ============================================================================
// RUN ALL TUTORIAL TESTS
// ============================================================================

static void run_tutorial_tests(s_engine_handle_t* engine) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           TUTORIAL TEST SUITE                                  ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    
    // Engine-allocated blackboard tests
    test_blackboard_access(engine);
    test_slot_access(engine);
    test_array_access(engine);
    test_nested_access(engine);
    test_constants(engine);
    
    // External blackboard tests
    test_external_blackboard(engine);
    test_shared_blackboard(engine);
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           ALL TUTORIAL TESTS COMPLETE                          ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main(int argc, char* argv[]) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           S-EXPRESSION ENGINE TUTORIAL TEST                    ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    
    (void)argc;
    (void)argv;
    
    // Setup allocator
    s_expr_allocator_t alloc = {
        .malloc = simple_malloc,
        .free = simple_free,
        .ctx = NULL,
        .get_time = linux_get_time
    };
    
    s_engine_handle_t engine;
    bool result;
    
    // User function registration list
    s_engine_user_register_fn user_fns[] = {
        register_black_board_functions
    };
    
    // ========================================================================
    // TEST 1: Load from ROM
    // ========================================================================
    
    printf("\n=== Loading module from ROM ===\n\n");
    result = s_engine_load_from_rom(
        &engine,
        &alloc,
        black_board_module_bin_32,
        BLACK_BOARD_MODULE_BIN_32_SIZE,
        debug_callback,
        1,
        user_fns
    );
    if (!result) {
        printf("❌ FATAL: Failed to load module from ROM\n");
        return 1;
    }
    
    // Set error callback (not part of load function)
    s_expr_module_set_error(&engine.module, error_callback);
    
    run_tutorial_tests(&engine);
    s_engine_free(&engine);
    
    // ========================================================================
    // TEST 2: Load from File (optional)
    // ========================================================================
    
    printf("\n\n=== Loading module from file ===\n\n");
    result = s_engine_load_from_file(
        &engine,
        &alloc,
        "black_board_32.bin",
        debug_callback,
        1,
        user_fns
    );
    if (!result) {
        printf("⚠️  WARNING: Could not load from file (may not exist)\n");
        printf("   This is OK if running without the binary file.\n");
    } else {
        // Set error callback
        s_expr_module_set_error(&engine.module, error_callback);
        
        run_tutorial_tests(&engine);
        s_engine_free(&engine);
    }
    
    printf("\n✅ All tests completed!\n\n");
    return 0;
}