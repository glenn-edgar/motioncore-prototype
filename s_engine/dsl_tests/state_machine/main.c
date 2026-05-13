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
#include "s_engine_init.h"
#include "s_engine_node.h"

#include "state_machine_test.h"
#include "state_machine_test_bin_32.h"
#include "state_machine_test_records.h"

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void register_user_functions(s_engine_handle_t* engine);

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
// DEBUG CALLBACK
// ============================================================================

static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst;
    printf("  [DEBUG] %s\n", msg);
}

// ============================================================================
// RESULT HELPERS
// ============================================================================

static const char* result_to_str(s_expr_result_t r) {
    switch (r) {
        // Application result codes (0-5)
        case SE_CONTINUE:               return "CONTINUE";
        case SE_HALT:                   return "HALT";
        case SE_TERMINATE:              return "TERMINATE";
        case SE_RESET:                  return "RESET";
        case SE_DISABLE:                return "DISABLE";
        case SE_SKIP_CONTINUE:          return "SKIP_CONTINUE";
        
        // Function result codes (6-11)
        case SE_FUNCTION_CONTINUE:      return "FUNCTION_CONTINUE";
        case SE_FUNCTION_HALT:          return "FUNCTION_HALT";
        case SE_FUNCTION_TERMINATE:     return "FUNCTION_TERMINATE";
        case SE_FUNCTION_RESET:         return "FUNCTION_RESET";
        case SE_FUNCTION_DISABLE:       return "FUNCTION_DISABLE";
        case SE_FUNCTION_SKIP_CONTINUE: return "FUNCTION_SKIP_CONTINUE";
        
        // Pipeline result codes (12-17)
        case SE_PIPELINE_CONTINUE:      return "PIPELINE_CONTINUE";
        case SE_PIPELINE_HALT:          return "PIPELINE_HALT";
        case SE_PIPELINE_TERMINATE:     return "PIPELINE_TERMINATE";
        case SE_PIPELINE_RESET:         return "PIPELINE_RESET";
        case SE_PIPELINE_DISABLE:       return "PIPELINE_DISABLE";
        case SE_PIPELINE_SKIP_CONTINUE: return "PIPELINE_SKIP_CONTINUE";
        
        default:                        return "UNKNOWN";
    }
}

static const char* result_scope_str(s_expr_result_t r) {
    if (r >= SE_PIPELINE_CONTINUE) return "PIPELINE";
    if (r >= SE_FUNCTION_CONTINUE) return "FUNCTION";
    return "LOCAL";
}

static s_expr_result_t result_base(s_expr_result_t r) {
    if (r >= SE_PIPELINE_CONTINUE) return r - SE_PIPELINE_CONTINUE;
    if (r >= SE_FUNCTION_CONTINUE) return r - SE_FUNCTION_CONTINUE;
    return r;
}

static bool result_stops_tick_loop(s_expr_result_t r) {
    s_expr_result_t base = result_base(r);
    return base == SE_DISABLE || base == SE_TERMINATE;
}

// ============================================================================
// UTC REALTIME TIMESTAMP
// ============================================================================

static double utc_realtime(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ============================================================================
// USER FUNCTION REGISTRATION
// ============================================================================

extern void state_machine_test_register_all(s_expr_module_t* module);

static void register_user_functions(s_engine_handle_t* engine) {
    state_machine_test_register_all(&engine->module);
}

// ============================================================================
// STATE MACHINE TEST
// ============================================================================

static void test_state_machine(s_engine_handle_t* engine) {
    printf("\n=== STATE MACHINE TEST ===\n\n");
    
    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &engine->module,
        STATE_MACHINE_TEST_HASH,
        0
    );
    
    if (!tree) {
        printf("FAILED: Could not create tree\n");
        return;
    }
    
    for (int i = 0; i < 500; i++) {
        s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        printf("Tick %3d: %s\n", i + 1, result_to_str(result));
        if (result == SE_FUNCTION_TERMINATE) {
            printf("✅ PASSED: Expected SE_FUNCTION_TERMINATE, got %s\n", result_to_str(result));
            break;
        }
    }
    
    
    s_expr_tree_free(tree);
}
// ============================================================================
// RUN ALL TESTS
// ============================================================================

static void run_state_machine_tests(s_engine_handle_t* engine) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           STATE MACHINE TEST SUITE                             ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    
    test_state_machine(engine);
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           ALL STATE MACHINE TESTS COMPLETE                     ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main(int argc, char* argv[]) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           S-EXPRESSION ENGINE STATE MACHINE TEST               ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    
    (void)argc;
    (void)argv;
    
    s_expr_allocator_t alloc = {
        .malloc = simple_malloc,
        .free = simple_free,
        .ctx = NULL,
        .get_time = utc_realtime
    };
    
    s_engine_handle_t engine;
    
    s_engine_user_register_fn user_fns[] = {
        register_user_functions
    };
    size_t user_fn_count = sizeof(user_fns) / sizeof(user_fns[0]);
    
    // ========================================================================
    // TEST 1: Load from ROM
    // ========================================================================
    
    printf("\n=== Loading module from ROM ===\n\n");
    
    bool loaded = s_engine_load_from_rom(
        &engine,
        &alloc,
        state_machine_test_module_bin_32,
        STATE_MACHINE_TEST_MODULE_BIN_32_SIZE,
        debug_callback,
        user_fn_count,
        user_fns
    );
    
    if (!loaded) {
        printf("❌ FATAL: Failed to load module from ROM\n");
        return 1;
    }
    
    printf("✅ Engine loaded from ROM\n");
    run_state_machine_tests(&engine);
    s_engine_free(&engine);
    
    // ========================================================================
    // TEST 2: Load from File (optional)
    // ========================================================================
    
    printf("\n\n=== Loading module from file ===\n\n");
    
    loaded = s_engine_load_from_file(
        &engine,
        &alloc,
        "state_machine_test_32.bin",
        debug_callback,
        user_fn_count,
        user_fns
    );
    
    if (!loaded) {
        printf("⚠️  WARNING: Could not load from file (may not exist)\n");
        printf("   This is OK if running without the binary file.\n");
    } else {
        printf("✅ Engine loaded from file\n");
        run_state_machine_tests(&engine);
        s_engine_free(&engine);
    }
    
    printf("\n✅ All tests completed!\n\n");
    return 0;
}