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

#include "return_tests.h"
#include "return_tests_bin_32.h"

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
// TEST RESULT TRACKING
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

// ============================================================================
// INDIVIDUAL TEST FUNCTIONS
// ============================================================================

static int tests_passed = 0;
static int tests_failed = 0;

static void test_return_code(s_engine_handle_t* engine, uint32_t tree_hash, const char* test_name, s_expr_result_t expected) {
    printf("Testing %s...\n", test_name);
    
    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &engine->module,
        tree_hash,
        0
    );
    if (!tree) {
        printf("  ❌ FAILED: Could not create tree (hash=0x%08X)\n", tree_hash);
        tests_failed++;
        return;
    }
    
    s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    
    if (result == expected) {
        printf("  ✅ PASS: %s (%d)\n", result_to_str(result), result);
        tests_passed++;
    } else {
        printf("  ❌ FAIL: got %s (%d), expected %s (%d)\n", 
               result_to_str(result), result,
               result_to_str(expected), expected);
        tests_failed++;
    }
    
    s_expr_tree_free(tree);
}

// ============================================================================
// RUN ALL RETURN VALUE TESTS
// ============================================================================

static void run_return_value_tests(s_engine_handle_t* engine) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                    RETURN VALUE TESTS                          ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    
    tests_passed = 0;
    tests_failed = 0;
    
    // ========================================================================
    // APPLICATION RESULT CODES (0-5)
    // ========================================================================
    printf("--- Application Result Codes (0-5) ---\n\n");
    
    test_return_code(engine, RETURN_CONTINUE_TEST_HASH, 
                     "SE_CONTINUE", SE_CONTINUE);
    
    test_return_code(engine, RETURN_HALT_TEST_HASH, 
                     "SE_HALT", SE_HALT);
    
    test_return_code(engine, RETURN_TERMINATE_TEST_HASH, 
                     "SE_TERMINATE", SE_TERMINATE);
    
    test_return_code(engine, RETURN_RESET_TEST_HASH, 
                     "SE_RESET", SE_RESET);
    
    test_return_code(engine, RETURN_DISABLE_TEST_HASH, 
                     "SE_DISABLE", SE_DISABLE);
    
    test_return_code(engine, RETURN_SKIP_CONTINUE_TEST_HASH, 
                     "SE_SKIP_CONTINUE", SE_SKIP_CONTINUE);
    
    // ========================================================================
    // FUNCTION RESULT CODES (6-11)
    // ========================================================================
    printf("\n--- Function Result Codes (6-11) ---\n\n");
    
    test_return_code(engine, RETURN_FUNCTION_CONTINUE_TEST_HASH, 
                     "SE_FUNCTION_CONTINUE", SE_FUNCTION_CONTINUE);
    
    test_return_code(engine, RETURN_FUNCTION_HALT_TEST_HASH, 
                     "SE_FUNCTION_HALT", SE_FUNCTION_HALT);
    
    test_return_code(engine, RETURN_FUNCTION_TERMINATE_TEST_HASH, 
                     "SE_FUNCTION_TERMINATE", SE_FUNCTION_TERMINATE);
    
    test_return_code(engine, RETURN_FUNCTION_RESET_TEST_HASH, 
                     "SE_FUNCTION_RESET", SE_FUNCTION_RESET);
    
    test_return_code(engine, RETURN_FUNCTION_DISABLE_TEST_HASH, 
                     "SE_FUNCTION_DISABLE", SE_FUNCTION_DISABLE);
    
    test_return_code(engine, RETURN_FUNCTION_SKIP_CONTINUE_TEST_HASH, 
                     "SE_FUNCTION_SKIP_CONTINUE", SE_FUNCTION_SKIP_CONTINUE);
    
    // ========================================================================
    // PIPELINE RESULT CODES (12-17)
    // ========================================================================
    printf("\n--- Pipeline Result Codes (12-17) ---\n\n");
    
    test_return_code(engine, RETURN_PIPELINE_CONTINUE_TEST_HASH, 
                     "SE_PIPELINE_CONTINUE", SE_PIPELINE_CONTINUE);
    
    test_return_code(engine, RETURN_PIPELINE_HALT_TEST_HASH, 
                     "SE_PIPELINE_HALT", SE_PIPELINE_HALT);
    
    test_return_code(engine, RETURN_PIPELINE_TERMINATE_TEST_HASH, 
                     "SE_PIPELINE_TERMINATE", SE_PIPELINE_TERMINATE);
    
    test_return_code(engine, RETURN_PIPELINE_RESET_TEST_HASH, 
                     "SE_PIPELINE_RESET", SE_PIPELINE_RESET);
    
    test_return_code(engine, RETURN_PIPELINE_DISABLE_TEST_HASH, 
                     "SE_PIPELINE_DISABLE", SE_PIPELINE_DISABLE);
    
    test_return_code(engine, RETURN_PIPELINE_SKIP_CONTINUE_TEST_HASH, 
                     "SE_PIPELINE_SKIP_CONTINUE", SE_PIPELINE_SKIP_CONTINUE);
    
    // ========================================================================
    // SUMMARY
    // ========================================================================
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                        TEST SUMMARY                            ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║  Passed: %2d                                                    ║\n", tests_passed);
    printf("║  Failed: %2d                                                    ║\n", tests_failed);
    printf("║  Total:  %2d                                                    ║\n", tests_passed + tests_failed);
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    
    if (tests_failed == 0) {
        printf("\n✅ ALL TESTS PASSED\n\n");
    } else {
        printf("\n❌ SOME TESTS FAILED\n\n");
    }
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main(int argc, char* argv[]) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           S-EXPRESSION ENGINE TEST SUITE                       ║\n");
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
    
    // ========================================================================
    // TEST: Load from ROM
    // ========================================================================
    printf("\n\nLoading module from ROM...\n\n");
    result = s_engine_load_from_rom(
        &engine,
        &alloc,
        return_tests_module_bin_32,
        RETURN_TESTS_MODULE_BIN_32_SIZE,
        debug_callback,
        0,
        NULL
    );
    if (!result) {
        printf("❌ FATAL: Failed to load module from ROM\n");
        return 1;
    }
    
    run_return_value_tests(&engine);
    s_engine_free(&engine);
    
    // ========================================================================
    // TEST: Load from file
    // ========================================================================
    printf("\n\nLoading module from file...\n\n");
    result = s_engine_load_from_file(
        &engine,
        &alloc,
        "return_tests_32.bin",
        debug_callback,
        0,
        NULL
    );
    if (!result) {
        printf("❌ FATAL: Failed to load module from file\n");
        return 1;
    }
    
    run_return_value_tests(&engine);
    s_engine_free(&engine);

    return 0;
}