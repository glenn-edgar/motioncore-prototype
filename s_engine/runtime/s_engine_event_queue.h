// ============================================================================
// s_engine_event_queue.h
// Per-Tree Event Queue Interface
// ============================================================================

#ifndef S_ENGINE_EVENT_QUEUE_H
#define S_ENGINE_EVENT_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "s_engine_types.h"

// Initialize event queue (zero head and count)
void s_expr_event_queue_init(s_expr_tree_instance_t* inst);

// Destroy event queue (zero head and count)
void s_expr_event_queue_destroy(s_expr_tree_instance_t* inst);

// Return number of queued events
uint16_t s_expr_event_queue_count(s_expr_tree_instance_t* inst);

// Push event onto queue tail. EXCEPTION if queue is full.
void s_expr_event_push(
    s_expr_tree_instance_t* inst,
    uint16_t tick_type,
    uint16_t event_id,
    void* event_data
);

// Pop event from queue head. EXCEPTION if queue is empty.
// All output pointers must be non-NULL.
void s_expr_event_pop(
    s_expr_tree_instance_t* inst,
    uint16_t* tick_type,
    uint16_t* event_id,
    void** event_data
);

// Clear all queued events
void s_expr_event_queue_clear(s_expr_tree_instance_t* inst);

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_EVENT_QUEUE_H