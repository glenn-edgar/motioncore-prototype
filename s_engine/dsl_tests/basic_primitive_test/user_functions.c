#include "basic_primitive_test_user_functions.h"
#include "s_engine_types.h"
#include "s_engine_exception.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define EVENT_BIT0_RISE     (1U << 0)
#define EVENT_BIT0_FALL     (1U << 1)
#define EVENT_BITS12_RISE   (1U << 2)
#define EVENT_BITS12_FALL   (1U << 3)
#define EVENT_BITS34_RISE   (1U << 4)
#define EVENT_BITS34_FALL   (1U << 5)
#define EVENT_BIT5_CLEAR    (1U << 6)
#define EVENT_BIT5_SET      (1U << 7)

// Globals
extern uint32_t g_trigger_events;  // Event tracking flags
extern uint32_t g_bitmap ;          // Bitmap for test_bit predicate

void on_bit0_rise(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BIT0_RISE\n");
    g_trigger_events |= EVENT_BIT0_RISE;
}

void on_bit0_fall(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BIT0_FALL\n");
    g_trigger_events |= EVENT_BIT0_FALL;
}

void on_bits_12_rise(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BITS_12_RISE (bit1 AND bit2)\n");
    g_trigger_events |= EVENT_BITS12_RISE;
}

void on_bits_12_fall(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BITS_12_FALL (bit1 AND bit2)\n");
    g_trigger_events |= EVENT_BITS12_FALL;
}

void on_bits_34_rise(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BITS_34_RISE (bit3 OR bit4)\n");
    g_trigger_events |= EVENT_BITS34_RISE;
}

void on_bits_34_fall(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BITS_34_FALL (bit3 OR bit4)\n");
    g_trigger_events |= EVENT_BITS34_FALL;
}

void on_bit5_clear(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BIT5_CLEAR (NOT bit5 went true)\n");
    g_trigger_events |= EVENT_BIT5_CLEAR;
}

void on_bit5_set(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    
    printf("  >> ON_BIT5_SET (NOT bit5 went false)\n");
    g_trigger_events |= EVENT_BIT5_SET;
}

// test_bit reads from user_ctx (which points to g_bitmap)
bool test_bit(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 1) {
        EXCEPTION("test_bit: need bit index");
        return false;
    }
    
    uint32_t* bitmap = (uint32_t*)inst->user_ctx;
    if (!bitmap) {
        EXCEPTION("test_bit: no bitmap in user_ctx");
        return false;
    }
    
    int32_t bit_index = params[0].int_val;
    if (bit_index < 0 || bit_index > 31) {
        EXCEPTION("test_bit: bit index out of range");
        return false;
    }
    
    bool result = (*bitmap & (1U << bit_index)) != 0;
    return result;
}