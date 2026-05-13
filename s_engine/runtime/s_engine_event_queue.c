// ============================================================================
// s_engine_event_queue.c
// Per-Tree Event Queue Implementation
// Circular buffer with head/count management
// ============================================================================

#include "s_engine_event_queue.h"
#include "s_engine_exception.h"

void s_expr_event_queue_init(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_event_queue_init: NULL instance");
        return;
    }
    inst->event_queue_head = 0;
    inst->event_queue_count = 0;
}

void s_expr_event_queue_destroy(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_event_queue_destroy: NULL instance");
        return;
    }
    inst->event_queue_head = 0;
    inst->event_queue_count = 0;
}

uint16_t s_expr_event_queue_count(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_event_queue_count: NULL instance");
        return 0;
    }
    return inst->event_queue_count;
}

void s_expr_event_push(
    s_expr_tree_instance_t* inst,
    uint16_t tick_type,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_event_push: NULL instance");
        return;
    }
    if (inst->event_queue_count >= S_EXPR_EVENT_QUEUE_SIZE) {
        EXCEPTION("s_expr_event_push: queue full");
        return;
    }
    
    uint8_t tail = (inst->event_queue_head + inst->event_queue_count) % S_EXPR_EVENT_QUEUE_SIZE;
    inst->event_queue[tail].tick_type = tick_type;
    inst->event_queue[tail].event_id = event_id;
    inst->event_queue[tail].event_data = event_data;
    inst->event_queue_count++;
}

void s_expr_event_pop(
    s_expr_tree_instance_t* inst,
    uint16_t* tick_type,
    uint16_t* event_id,
    void** event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_event_pop: NULL instance");
        return;
    }
    if (!tick_type || !event_id || !event_data) {
        EXCEPTION("s_expr_event_pop: NULL output pointer");
        return;
    }
    if (inst->event_queue_count == 0) {
        EXCEPTION("s_expr_event_pop: queue empty");
        return;
    }
    
    *tick_type = inst->event_queue[inst->event_queue_head].tick_type;
    *event_id = inst->event_queue[inst->event_queue_head].event_id;
    *event_data = inst->event_queue[inst->event_queue_head].event_data;
    
    inst->event_queue_head = (inst->event_queue_head + 1) % S_EXPR_EVENT_QUEUE_SIZE;
    inst->event_queue_count--;
}

void s_expr_event_queue_clear(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_event_queue_clear: NULL instance");
        return;
    }
    inst->event_queue_head = 0;
    inst->event_queue_count = 0;
}