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
#include "loop_test.h"
#include "loop_test_bin_32.h"




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

// ============================================================================
// DISPATCH TEST
// ============================================================================

static void test_dispatch(s_engine_handle_t* engine) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║    LOOP TEST                           ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    printf("\nTesting dispatch with tick loop...\n");
    
    s_expr_tree_instance_t* tree = s_engine_create_tree_by_hash(
        engine,
        LOOP_TEST_HASH,
        0
    );
    
    if (!tree) {
        printf("  ❌ FAILED: Could not create tree (hash=0x%08X)\n", LOOP_TEST_HASH); 
        return;
    }
    
    int tick_count = 0;
    int max_ticks = 500;
    s_expr_result_t result;
    
    printf("\n  Running tick loop...\n");
    
    do {
        result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        tick_count++;
        
        printf("------------------------>    Tick %3d: result=%s\n", tick_count, result_to_str(result));
        
        // Process queued events
        uint16_t event_count = s_expr_event_queue_count(tree);
        //printf("------------------------>      Event count: %d\n", event_count);
        while (event_count > 0) {
            uint16_t tick_type;
            uint16_t event_id;
            void* event_data;
            
            s_expr_event_pop(tree, &tick_type, &event_id, &event_data);
            //printf("-------------------------------->      Event: tick_type=%d, event_id=%d, event_data=%p\n", 
                   //tick_type, event_id, event_data);
            
            // Save, set, execute, restore
            uint16_t saved_tick_type = tree->tick_type;
            tree->tick_type = tick_type;
            
            s_expr_result_t event_result = s_expr_node_tick(tree, event_id, event_data);
            
            tree->tick_type = saved_tick_type;
            
            //printf("-------------------------------->      Event result: %s\n", result_to_str(event_result));
            
            if (result_is_complete(event_result)) {
                result = event_result;
                break;
            }
            
            event_count = s_expr_event_queue_count(tree);
        }
        
    } while (!result_is_complete(result) && tick_count < max_ticks);
    
    printf("\n  Total ticks: %d\n", tick_count);
    printf("  Final result: %s\n", result_to_str(result));
    
    if (result_is_terminate(result)) {
        printf("\n  ✅ PASSED - Tree terminated normally\n");
    } else if (tick_count >= max_ticks) {
        printf("\n  ❌ FAILED - Max ticks exceeded without termination\n");
    } else if (result_is_complete(result)) {
        printf("\n  ✅ PASSED - Tree completed (disabled)\n");
    } else {
        printf("\n  ❌ FAILED - Unexpected result\n");
    }
    
    s_expr_tree_free(tree);
    
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================

int main(int argc, char* argv[]) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           S-EXPRESSION ENGINE DISPATCH TEST                    ║\n");
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
   
    // ========================================================================
    // TEST 1: Load from ROM
    // ========================================================================
    
    printf("\n=== Loading module from ROM ===\n\n");
    
    bool result = s_engine_load_from_rom(
        &engine,
        &alloc,
        loop_test_module_bin_32,
        LOOP_TEST_MODULE_BIN_32_SIZE,
        debug_callback,
        0,
        NULL
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
    
    test_dispatch(&engine);
    s_engine_free(&engine);
    
    // ========================================================================
    // TEST 2: Load from File (optional)
    // ========================================================================
    
    printf("\n\n=== Loading module from file ===\n\n");
    
    result = s_engine_load_from_file(
        &engine,
        &alloc,
        "loop_test_32.bin",
        debug_callback,
        0,
        NULL
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
        
        test_dispatch(&engine);
        s_engine_free(&engine);
    }
    
    printf("\n✅ All tests completed!\n\n");
    return 0;
}