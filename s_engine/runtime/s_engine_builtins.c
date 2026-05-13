// ============================================================================
// s_engine_builtins.c
// Built-in S-Expression Engine Functions Implementation - Version 5.2
//
// VERSION 5.2 CHANGES:
//   - Added dictionary navigation helpers
//   - Added se_string_dispatch (string-based dispatch with hash lookup)
//   - Added se_hash_dispatch (dispatch on pre-computed hash)
//   - Added se_named_state_machine (state machine with string states)
//   - Added se_named_event_dispatch (event dispatch with string names)
//   - Added field comparison predicates
//   - Added field operation oneshots
// ============================================================================

#include "s_engine_builtins.h"
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_types.h"
#include "s_engine_node.h"
#include "s_engine_exception.h"
//#include "s_engine_list_dictionary_support.h"
//#include "s_engine_stack_functions.h"
#include "s_engine_event_queue.h"
#include "s_engine_stack.h"
#include "se_dict_string.h"
#include "se_dict_hash.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ============================================================================
// FUNCTION TABLES
// ============================================================================
#include "s_engine_builtins_shared_helpers.h"
#include "s_engine_builtins_pred.h"
#include "s_engine_builtins_delays.h"
#include "s_engine_builtins_flow_control.h"
#include "s_engine_builtins_dispatch.h"
#include "s_engine_builtins_verify.h"
#include "s_engine_builtins_oneshot.h"
#include "s_engine_builtins_return_codes.h"
#include "s_engine_builtins_quads.h"
#include "s_engine_builtins_dict.h"
#include "s_engine_builtins_stack.h"
#include "s_engine_builtins_spawn.h"


static s_expr_fn_entry_t builtin_oneshot_entries[] = {
    { SE_LOG_HASH, (void*)se_log },
    { SE_LOG_INT_HASH, (void*)se_log_int },
    { SE_LOG_FLOAT_HASH, (void*)se_log_float },
    { SE_LOG_FIELD_HASH, (void*)se_log_field },
    { SE_SET_FIELD_HASH, (void*)se_set_field },
    { SE_SET_FIELD_FLOAT_HASH, (void*)se_set_field_float },
    { SE_INC_FIELD_HASH, (void*)se_inc_field },
    { SE_DEC_FIELD_HASH, (void*)se_dec_field },
    { SE_SET_HASH_HASH, (void*)se_set_hash },// Function table entries
    
    
    {SE_DICT_EXTRACT_INT_HASH,     (void*)se_dict_extract_int},
    {SE_DICT_EXTRACT_UINT_HASH,    (void*)se_dict_extract_uint},
    {SE_DICT_EXTRACT_FLOAT_HASH,   (void*)se_dict_extract_float},
    {SE_DICT_EXTRACT_BOOL_HASH,    (void*)se_dict_extract_bool},
    {SE_DICT_EXTRACT_HASH_HASH,    (void*)se_dict_extract_hash},
    {SE_DICT_EXTRACT_INT_H_HASH,   (void*)se_dict_extract_int_h},
    {SE_DICT_EXTRACT_UINT_H_HASH,  (void*)se_dict_extract_uint_h},
    {SE_DICT_EXTRACT_FLOAT_H_HASH, (void*)se_dict_extract_float_h},
    {SE_DICT_EXTRACT_BOOL_H_HASH,  (void*)se_dict_extract_bool_h},
    {SE_DICT_EXTRACT_HASH_H_HASH,  (void*)se_dict_extract_hash_h},

    {SE_DICT_STORE_PTR_HASH,     (void*)se_dict_store_ptr},
    {SE_DICT_STORE_PTR_H_HASH,   (void*)se_dict_store_ptr_h},
    { SE_QUEUE_EVENT_HASH,       (void*)se_queue_event },     // SE_QUEUE_EVENT
    {SE_PUSH_STACK_HASH,         (void*)se_push_stack},         // SE_PUSH_STACK
    {SE_QUAD_HASH,               (void*)se_quad},               // SE_QUAD
    {SE_LOG_STACK_HASH,          (void*)se_log_stack}, 
    {SE_LOAD_DICTIONARY_HASH,      (void*)se_load_dictionary},         // SE_LOG_STACK
    {SE_LOAD_FUNCTION_DICT_HASH,      (void*)se_load_function_dict},     
    {SE_LOAD_FUNCTION_HASH,      (void*)se_load_function},    // SE_LOG_STACK
    {SE_SET_EXTERNAL_FIELD_HASH, (void*)se_set_external_field},

};

static s_expr_fn_entry_t builtin_main_entries[] = {
    //{ SE_PIPELINE_HASH, (void*)se_pipeline },
    { SE_TICK_DELAY_HASH, (void*)se_tick_delay },
    { SE_TIME_DELAY_HASH, (void*)se_time_delay },
    { SE_WAIT_EVENT_HASH, (void*)se_wait_event },
    { SE_NOP_HASH, (void*)se_nop },
    { SE_IF_THEN_ELSE_HASH, (void*)se_if_then_else },
    { SE_TRIGGER_ON_CHANGE_HASH, (void*)se_trigger_on_change },
    { SE_STATE_MACHINE_HASH, (void*)se_state_machine },
    //{ SE_STATE_ACTIONS_HASH, (void*)se_state_actions },
    { SE_FIELD_DISPATCH_HASH, (void*)se_field_dispatch },
    { SE_EVENT_DISPATCH_HASH, (void*)se_event_dispatch },
    { SE_FUNCTION_INTERFACE_HASH, (void*)se_function_interface },
    { SE_SEQUENCE_HASH, (void*)se_sequence },
    { SE_SEQUENCE_ONCE_HASH, (void*)se_sequence_once },
    { SE_FORK_HASH, (void*)se_fork },
    { SE_FORK_JOIN_HASH, (void*)se_fork_join },
    { SE_CHAIN_FLOW_HASH, (void*)se_chain_flow },
    
    { SE_WHILE_HASH, (void*)se_while },
    { SE_COND_HASH, (void*)se_cond },
    { SE_VERIFY_AND_CHECK_ELAPSED_TIME_HASH, (void*)se_verify_and_check_elapsed_time },
    { SE_VERIFY_AND_CHECK_ELAPSED_EVENTS_HASH, (void*)se_verify_and_check_elapsed_events },
    { SE_VERIFY_HASH, (void*)se_verify },
    { SE_WAIT_HASH, (void*)se_wait },
    { SE_WAIT_TIMEOUT_HASH, (void*)se_wait_timeout },
    { SE_STACK_FRAME_INSTANCE_HASH, (void*)se_stack_frame_instance },
    { SE_FRAME_ALLOCATE_HASH, (void*)se_frame_allocate },
    { SE_FRAME_FREE_HASH, (void*)se_frame_free },
    // NEW v5.2: Dictionary-based dispatch
    { SE_SPAWN_AND_TICK_TREE_HASH, (void*)se_spawn_and_tick_tree },
    { SE_EXEC_FN_HASH, (void*)se_exec_fn },
    { SE_EXEC_DICT_INTERNAL_HASH, (void*)se_exec_dict_internal },
    { SE_EXEC_DICT_DISPATCH_HASH, (void*)se_exec_dict_dispatch },
    { SE_EXEC_DICT_FN_PTR_HASH, (void*)se_exec_dict_fn_ptr },
    // Result code functions
    { SE_RETURN_CONTINUE_HASH, (void*)se_return_continue },
    { SE_RETURN_HALT_HASH, (void*)se_return_halt },
    { SE_RETURN_TERMINATE_HASH, (void*)se_return_terminate },
    { SE_RETURN_RESET_HASH, (void*)se_return_reset },
    { SE_RETURN_DISABLE_HASH, (void*)se_return_disable },
    { SE_RETURN_SKIP_CONTINUE_HASH, (void*)se_return_skip_continue },

    { SE_RETURN_FUNCTION_CONTINUE_HASH, (void*)se_return_function_continue },
    { SE_RETURN_FUNCTION_HALT_HASH, (void*)se_return_function_halt },
    { SE_RETURN_FUNCTION_RESET_HASH, (void*)se_return_function_reset },
    { SE_RETURN_FUNCTION_TERMINATE_HASH, (void*)se_return_function_terminate },
    { SE_RETURN_FUNCTION_DISABLE_HASH, (void*)se_return_function_disable },
    { SE_RETURN_FUNCTION_SKIP_CONTINUE_HASH, (void*)se_return_function_skip_continue },

    // Pipeline result code functions
    { SE_RETURN_PIPELINE_CONTINUE_HASH, (void*)se_return_pipeline_continue },
    { SE_RETURN_PIPELINE_TERMINATE_HASH, (void*)se_return_pipeline_terminate },
    { SE_RETURN_PIPELINE_RESET_HASH, (void*)se_return_pipeline_reset },
    { SE_RETURN_PIPELINE_DISABLE_HASH, (void*)se_return_pipeline_disable },
    { SE_RETURN_PIPELINE_SKIP_CONTINUE_HASH, (void*)se_return_pipeline_skip_continue },
    { SE_RETURN_PIPELINE_HALT_HASH, (void*)se_return_pipeline_halt },
    { SE_SPAWN_TREE_HASH, (void*)se_spawn_tree },
    { SE_TICK_TREE_HASH, (void*)se_tick_tree },
};

static s_expr_fn_entry_t builtin_pred_entries[] = {
    { SE_PRED_AND_HASH, (void*)se_pred_and },
    { SE_PRED_OR_HASH, (void*)se_pred_or },
    { SE_PRED_NOT_HASH, (void*)se_pred_not },
    { SE_PRED_NOR_HASH, (void*)se_pred_nor },
    { SE_PRED_NAND_HASH, (void*)se_pred_nand },
    { SE_PRED_XOR_HASH, (void*)se_pred_xor },
    { SE_TRUE_HASH, (void*)se_true },
    { SE_FALSE_HASH, (void*)se_false },
    { SE_CHECK_EVENT_HASH, (void*)se_check_event },
    
    // Field comparison predicates
    { SE_FIELD_EQ_HASH, (void*)se_field_eq },
    { SE_FIELD_NE_HASH, (void*)se_field_ne },
    { SE_FIELD_GT_HASH, (void*)se_field_gt },
    { SE_FIELD_GE_HASH, (void*)se_field_ge },
    { SE_FIELD_LT_HASH, (void*)se_field_lt },
    { SE_FIELD_LE_HASH, (void*)se_field_le },
    { SE_FIELD_IN_RANGE_HASH, (void*)se_field_in_range },


    { SE_FIELD_INCREMENT_AND_TEST_HASH, (void*)se_field_increment_and_test },
   
    { SE_STATE_INCREMENT_AND_TEST_HASH, (void*)se_state_increment_and_test },
    { SE_P_QUAD_HASH, (void*)se_p_quad },

    
};

static const s_expr_fn_table_t builtin_oneshot_table = {
    .entries = builtin_oneshot_entries,
    .count = sizeof(builtin_oneshot_entries) / sizeof(builtin_oneshot_entries[0])
};

static const s_expr_fn_table_t builtin_main_table = {
    .entries = builtin_main_entries,
    .count = sizeof(builtin_main_entries) / sizeof(builtin_main_entries[0])
};

static const s_expr_fn_table_t builtin_pred_table = {
    .entries = builtin_pred_entries,
    .count = sizeof(builtin_pred_entries) / sizeof(builtin_pred_entries[0])
};

// ============================================================================
// TABLE ACCESSORS
// ============================================================================

const s_expr_fn_table_t* s_engine_builtin_oneshot_table(void) {
    return &builtin_oneshot_table;
}

const s_expr_fn_table_t* s_engine_builtin_main_table(void) {
    return &builtin_main_table;
}

const s_expr_fn_table_t* s_engine_builtin_pred_table(void) {
    return &builtin_pred_table;
}

















