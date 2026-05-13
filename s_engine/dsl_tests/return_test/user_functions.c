#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_builtins.h"
#include "cfl_exception.h"
#include "s_engine_node.h"
#include  "s_engine_types.h"
#include "s_engine_list_dictionary_support.h"
#include <stdio.h>
#include <string.h>


void cfl_disable_children(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(inst);
    UNUSED(params); UNUSED(param_count);
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    printf("\n ---------------> cfl_disable_children\n\n");
}

void cfl_enable_child(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(inst);
    
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    if (param_count < 1) {
        EXCEPTION("cfl_enable_child: not enough parameters");
        return;
    }
    uint8_t opcode = params[0].type & S_EXPR_OPCODE_MASK;
    printf("cfl_enable_child: opcode=%d\n", opcode);
    
    if (opcode !=(S_EXPR_PARAM_INT && S_EXPR_PARAM_UINT)) {   
       EXCEPTION("cfl_enable_child: invalid parameter type");
    } 
    
    uint16_t child_index = params[0].int_val;
    printf("\n\n---------------------------------> cfl_enable_child: child_index=%d\n\n", child_index);
    

}