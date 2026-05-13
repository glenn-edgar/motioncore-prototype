#include "dispatch_test_user_functions.h"
#include <stdio.h>
#include <stdlib.h>

// Oneshot functions
void display_event_info(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count);
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    if( event_id == SE_EVENT_TICK)
    {
        return;
        //return;
    }
    printf("******************[display_event_info] Displaying event info\n");
    
    printf("******************[display_event_info] Event type: %d, Event ID: %d\n", event_type, event_id);
    uint16_t offset = (uint16_t)(size_t)event_data;
    printf("******************[display_event_info] Event data %d\n",  offset);
    float* value = ((float*)((uint8_t*)(inst)->blackboard + offset));
    printf("******************[display_event_info] Value: %f\n", *value);
    
}