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
#include "s_engine_event_queue.h"
#include "basic_primitive_test.h"
#include "basic_primitive_test_bin_32.h"


extern void basic_primitive_test_register_all(s_expr_module_t* module);

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

// Linux monotonic time
static double linux_get_time(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ============================================================================
// DEBUG CALLBACK
// ============================================================================

static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst;
    printf("  [DEBUG] %s\n", msg);
}

// ============================================================================
// USER FUNCTION REGISTRATION
// ============================================================================

static void register_user_functions(s_engine_handle_t* engine) {
    basic_primitive_test_register_all(&engine->module);
}

// ============================================================================
// RESULT HELPERS
// ============================================================================

static const char* result_to_str(s_expr_result_t r) {
    switch (r) {
        // APPLICATION RESULT CODES (0-5)
        case SE_CONTINUE:               return "CONTINUE";
        case SE_HALT:                   return "HALT";
        case SE_TERMINATE:              return "TERMINATE";
        case SE_RESET:                  return "RESET";
        case SE_DISABLE:                return "DISABLE";
        case SE_SKIP_CONTINUE:          return "SKIP_CONTINUE";
        
        // FUNCTION RESULT CODES (6-11)
        case SE_FUNCTION_CONTINUE:      return "FUNCTION_CONTINUE";
        case SE_FUNCTION_HALT:          return "FUNCTION_HALT";
        case SE_FUNCTION_TERMINATE:     return "FUNCTION_TERMINATE";
        case SE_FUNCTION_RESET:         return "FUNCTION_RESET";
        case SE_FUNCTION_DISABLE:       return "FUNCTION_DISABLE";
        case SE_FUNCTION_SKIP_CONTINUE: return "FUNCTION_SKIP_CONTINUE";
        
        // PIPELINE RESULT CODES (12-17)
        case SE_PIPELINE_CONTINUE:      return "PIPELINE_CONTINUE";
        case SE_PIPELINE_HALT:          return "PIPELINE_HALT";
        case SE_PIPELINE_TERMINATE:     return "PIPELINE_TERMINATE";
        case SE_PIPELINE_RESET:         return "PIPELINE_RESET";
        case SE_PIPELINE_DISABLE:       return "PIPELINE_DISABLE";
        case SE_PIPELINE_SKIP_CONTINUE: return "PIPELINE_SKIP_CONTINUE";
        
        default:                        return "UNKNOWN";
    }
}

static bool result_is_terminate(s_expr_result_t r) {
    return r == SE_TERMINATE || 
           r == SE_FUNCTION_TERMINATE || 
           r == SE_PIPELINE_TERMINATE;
}

static bool result_is_complete(s_expr_result_t r) {
    return r == SE_TERMINATE || 
           r == SE_FUNCTION_TERMINATE || 
           r == SE_PIPELINE_TERMINATE ||
           r == SE_DISABLE ||
           r == SE_FUNCTION_DISABLE ||
           r == SE_PIPELINE_DISABLE;
}

// Bitmask of which events fired

#define EVENT_BIT0_RISE     (1U << 0)
#define EVENT_BIT0_FALL     (1U << 1)
#define EVENT_BITS12_RISE   (1U << 2)
#define EVENT_BITS12_FALL   (1U << 3)
#define EVENT_BITS34_RISE   (1U << 4)
#define EVENT_BITS34_FALL   (1U << 5)
#define EVENT_BIT5_CLEAR    (1U << 6)
#define EVENT_BIT5_SET      (1U << 7)

uint32_t g_trigger_events = 0;  // Event tracking flags
uint32_t g_bitmap = 0;          // Bitmap for test_bit predicate

void reset_trigger_events(void) {
    g_trigger_events = 0;
}

uint32_t get_trigger_events(void) {
    return g_trigger_events;
}

static void test_trigger_on_change(s_engine_handle_t* engine) {
    
    printf("\n=== Test Trigger On Change ===\n");
    
    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &engine->module,
        BASIC_PRIMITIVE_TEST_HASH,
        0
    );
    if (!tree) {
        printf("  ❌ FAILED: Could not create tree\n");
        exit(1);
    }
    
    // Bitmap for predicates to read - SEPARATE from event tracking
    g_bitmap = 0;
    tree->user_ctx = &g_bitmap;
    g_trigger_events = 0;
    
    int test_pass = 1;
    
    // -------------------------------------------------------------------------
    // Initial tick - all triggers start with their initial state
    // Trigger 4 starts with initial_state=1 (NOT bit5, bit5=0 means pred=true)
    // -------------------------------------------------------------------------
    printf("\n--- Initial tick (bitmap=0x%08X) ---\n", g_bitmap);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    // No transitions expected on first tick (state matches initial)
    
    // -------------------------------------------------------------------------
    // Test 1: Set bit 0 -> should trigger ON_BIT0_RISE
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 0 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x01);
    g_bitmap |= (1U << 0);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BIT0_RISE)) {
        printf("  ❌ Expected BIT0_RISE\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 2: Clear bit 0 -> should trigger ON_BIT0_FALL
    // -------------------------------------------------------------------------
    printf("\n--- Clear bit 0 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap & ~0x01);
    g_bitmap &= ~(1U << 0);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BIT0_FALL)) {
        printf("  ❌ Expected BIT0_FALL\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 3: Set bit 1 only -> AND should not trigger (need both 1 and 2)
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 1 only (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x02);
    g_bitmap |= (1U << 1);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (get_trigger_events() & EVENT_BITS12_RISE) {
        printf("  ❌ Unexpected BITS12_RISE (only bit1 set)\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 4: Set bit 2 -> now AND is true, should trigger ON_BITS_12_RISE
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 2 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x04);
    g_bitmap |= (1U << 2);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BITS12_RISE)) {
        printf("  ❌ Expected BITS12_RISE\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 5: Clear bit 1 -> AND becomes false, should trigger ON_BITS_12_FALL
    // -------------------------------------------------------------------------
    printf("\n--- Clear bit 1 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap & ~0x02);
    g_bitmap &= ~(1U << 1);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BITS12_FALL)) {
        printf("  ❌ Expected BITS12_FALL\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 6: Set bit 3 -> OR becomes true, should trigger ON_BITS_34_RISE
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 3 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x08);
    g_bitmap |= (1U << 3);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BITS34_RISE)) {
        printf("  ❌ Expected BITS34_RISE\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 7: Set bit 4 also -> OR still true, no new trigger
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 4 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x10);
    g_bitmap |= (1U << 4);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (get_trigger_events() & (EVENT_BITS34_RISE | EVENT_BITS34_FALL)) {
        printf("  ❌ Unexpected BITS34 event (OR still true)\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 8: Clear bit 3 -> OR still true (bit 4 set), no trigger
    // -------------------------------------------------------------------------
    printf("\n--- Clear bit 3 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap & ~0x08);
    g_bitmap &= ~(1U << 3);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (get_trigger_events() & (EVENT_BITS34_RISE | EVENT_BITS34_FALL)) {
        printf("  ❌ Unexpected BITS34 event (OR still true via bit4)\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 9: Clear bit 4 -> OR now false, should trigger ON_BITS_34_FALL
    // -------------------------------------------------------------------------
    printf("\n--- Clear bit 4 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap & ~0x10);
    g_bitmap &= ~(1U << 4);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BITS34_FALL)) {
        printf("  ❌ Expected BITS34_FALL\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 10: Set bit 5 -> NOT bit5 becomes false, should trigger ON_BIT5_SET
    // (initial_state=1, so NOT bit5 starts true when bit5=0)
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 5 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x20);
    g_bitmap |= (1U << 5);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BIT5_SET)) {
        printf("  ❌ Expected BIT5_SET (NOT became false)\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 11: Clear bit 5 -> NOT bit5 becomes true, should trigger ON_BIT5_CLEAR
    // -------------------------------------------------------------------------
    printf("\n--- Clear bit 5 (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap & ~0x20);
    g_bitmap &= ~(1U << 5);
    reset_trigger_events();
    s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BIT5_CLEAR)) {
        printf("  ❌ Expected BIT5_CLEAR (NOT became true)\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 12: Set bit 0 AGAIN -> should trigger ON_BIT0_RISE again
    // This verifies the action path resets properly
    // -------------------------------------------------------------------------
    printf("\n--- Set bit 0 AGAIN (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap | 0x01);
    g_bitmap |= (1U << 0);
    reset_trigger_events();
    (void)s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BIT0_RISE)) {
        printf("  ❌ Expected BIT0_RISE on repeated trigger\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 13: Clear bit 0 AGAIN -> should trigger ON_BIT0_FALL again
    // -------------------------------------------------------------------------
    printf("\n--- Clear bit 0 AGAIN (bitmap=0x%08X -> 0x%08X) ---\n", g_bitmap, g_bitmap & ~0x01);
    g_bitmap &= ~(1U << 0);
    reset_trigger_events();
    (void)s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    printf("  events fired: 0x%02X\n", get_trigger_events());
    if (!(get_trigger_events() & EVENT_BIT0_FALL)) {
        printf("  ❌ Expected BIT0_FALL on repeated trigger\n");
        test_pass = 0;
    }
    
    // -------------------------------------------------------------------------
    // Test 14: Rapid toggle - multiple cycles
    // -------------------------------------------------------------------------
    printf("\n--- Rapid toggle bit 0 (3 cycles) ---\n");
    for (int cycle = 0; cycle < 3; cycle++) {
        // Rise
        g_bitmap |= (1U << 0);
        reset_trigger_events();
        (void)s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        if (!(get_trigger_events() & EVENT_BIT0_RISE)) {
            printf("  ❌ Cycle %d: Expected BIT0_RISE\n", cycle);
            test_pass = 0;
        }
        
        // Fall
        g_bitmap &= ~(1U << 0);
        reset_trigger_events();
        (void)s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        if (!(get_trigger_events() & EVENT_BIT0_FALL)) {
            printf("  ❌ Cycle %d: Expected BIT0_FALL\n", cycle);
            test_pass = 0;
        }
    }
    printf("  Completed 3 toggle cycles\n");
    
    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------
    if (test_pass) {
        printf("\n  ✅ PASSED: All edge triggers working correctly\n");
    } else {
        printf("\n  ❌ FAILED: Some edge triggers failed\n");
    }
    
    s_expr_tree_free(tree);
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main(int argc, char* argv[]) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           S-EXPRESSION ENGINE TRIGGER ON CHANGE TEST                    ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    
    (void)argc;
    (void)argv;
    
    s_expr_allocator_t alloc = {
        .malloc = simple_malloc,
        .free = simple_free,
        .ctx = NULL,
        .get_time = linux_get_time
    };
    
    s_engine_handle_t engine;
    s_engine_user_register_fn user_fns[] = { register_user_functions };
    
    // ========================================================================
    // TEST 1: Load from ROM
    // ========================================================================
    
    printf("\n=== Loading module from ROM ===\n\n");
    
    bool result = s_engine_load_from_rom(
        &engine,
        &alloc,
        basic_primitive_test_module_bin_32,
        BASIC_PRIMITIVE_TEST_MODULE_BIN_32_SIZE,
        debug_callback,
        1,
        user_fns
    );
    
    if (!result) {
        printf("❌ FATAL: Failed to load module from ROM: %s\n", s_engine_error_str(&engine));
        return 1;
    }
    
    printf("✅ Module loaded successfully\n");
    printf("   Trees:    %d\n", engine.module.def->tree_count);
    printf("   Records:  %d\n", engine.module.def->record_count);
    printf("   Strings:  %d\n", engine.module.def->string_count);
    printf("   Oneshot:  %d\n", engine.module.def->oneshot_count);
    printf("   Main:     %d\n", engine.module.def->main_count);
    printf("   Pred:     %d\n", engine.module.def->pred_count);
        
        test_trigger_on_change(&engine);
    s_engine_free(&engine);
    
    // ========================================================================
    // TEST 2: Load from File (optional)
    // ========================================================================
    
    printf("\n\n=== Loading module from file ===\n\n");
    
    result = s_engine_load_from_file(
        &engine,
        &alloc,
        "basic_primitive_test_32.bin",
        debug_callback,
        1,
        user_fns
    );
    
    if (!result) {
        printf("⚠️  WARNING: Could not load from file: %s\n", s_engine_error_str(&engine));
        printf("   This is OK if running without the binary file.\n");
    } else {
        printf("✅ Module loaded successfully\n");
        printf("   Trees:    %d\n", engine.module.def->tree_count);
        printf("   Records:  %d\n", engine.module.def->record_count);
        printf("   Strings:  %d\n", engine.module.def->string_count);
        printf("   Oneshot:  %d\n", engine.module.def->oneshot_count);
        printf("   Main:     %d\n", engine.module.def->main_count);
        printf("   Pred:     %d\n", engine.module.def->pred_count);
        
        test_trigger_on_change(&engine);
        s_engine_free(&engine);
    }
    
    printf("\n✅ All tests completed!\n\n");
    return 0;
}