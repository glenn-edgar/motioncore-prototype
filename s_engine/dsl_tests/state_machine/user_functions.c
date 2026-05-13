#include "state_machine_test_user_functions.h"
#include "s_engine_exception.h"
#include <stdio.h>

// Oneshot functions
void cfl_disable_children(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
){
(void)inst; (void)params; (void)param_count; (void)event_id; (void)event_data;
if (event_type == SE_EVENT_INIT) {
    ;
}
if (event_type == SE_EVENT_TERMINATE) {
   ;
}

  printf("cfl_disable_children\n");

}

void cfl_enable_child(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
){


(void)inst; (void)params; (void)param_count; (void)event_id; (void)event_data;
if (event_type == SE_EVENT_INIT) {
    return;
}
if (event_type == SE_EVENT_TERMINATE) {
    return;
}

if( param_count < 1) {
    EXCEPTION("cfl_enable_child: need at least one parameter");
}
if( ( params[0].type != S_EXPR_PARAM_INT) && ( params[0].type != S_EXPR_PARAM_UINT)) {
    EXCEPTION("cfl_enable_child: first parameter must be an integer or unsigned integer");
}

uint16_t child_index = params[0].int_val;
printf("cfl_enable_child: enabling child %d\n", child_index);

}