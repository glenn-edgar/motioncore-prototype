// ============================================================================
// s_engine_builtins.h
// Built-in S-Expression Engine Functions - Version 5.2
// ============================================================================

#ifndef S_ENGINE_BUILTINS_H
#define S_ENGINE_BUILTINS_H

#include "s_engine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BUILTIN FUNCTION TABLES
// Returns NULL if no functions of that type
// ============================================================================

const s_expr_fn_table_t* s_engine_builtin_oneshot_table(void);
const s_expr_fn_table_t* s_engine_builtin_main_table(void);
const s_expr_fn_table_t* s_engine_builtin_pred_table(void);

// ============================================================================
// DICTIONARY NAVIGATION HELPERS
// ============================================================================

// Find a key in a dictionary by hash value
// Returns pointer to OPEN_KEY param, or NULL if not found
// dict_param should point to OPEN_DICT
const s_expr_param_t* s_expr_dict_find_key(
    const s_expr_param_t* dict_param,
    uint32_t key_hash
);

// Get the contents of a dictionary key (between OPEN_KEY and CLOSE_KEY)
// key_param should point to OPEN_KEY
// Returns pointer to first content param, sets content_count
const s_expr_param_t* s_expr_key_contents(
    const s_expr_param_t* key_param,
    uint16_t* content_count
);

// Compute FNV-1a 32-bit hash (matches Lua implementation)
uint32_t s_expr_fnv1a_hash(const char* str);

// ============================================================================
// BUILTIN FUNCTION HASHES (uppercase names)
// ============================================================================

// Predicates
#define SE_PRED_AND_HASH          0x7C0DF5F3
#define SE_PRED_OR_HASH           0x0CF6212F
#define SE_PRED_NOT_HASH          0x217DEB8F
#define SE_PRED_NOR_HASH          0x1F7DE869
#define SE_PRED_NAND_HASH         0x067DC775
#define SE_PRED_XOR_HASH          0xB713B6A3
#define SE_TRUE_HASH              0x0C125BC2
#define SE_FALSE_HASH             0x77C35775
#define SE_CHECK_EVENT_HASH       0x80659F81
#define SE_CHECK_NAMED_EVENT_HASH 0x542BD82B
#define SE_LESS_THAN_INT_HASH     0xBFE88BCD
#define SE_GREATER_EQUAL_INT_HASH 0xBB057075
#define SE_P_QUAD_HASH              0x4D8ADF00

// Main functions
#define SE_FUNCTION_INTERFACE_HASH    0xC7FEA7F6
#define SE_TICK_DELAY_HASH            0x0C3460EB
#define SE_TIME_DELAY_HASH            0xA60CE767
#define SE_WAIT_EVENT_HASH            0xAD4917EC
#define SE_NOP_HASH                   0x080C2B37
#define SE_IF_THEN_ELSE_HASH          0x1E860193
#define SE_TRIGGER_ON_CHANGE_HASH     0x8374277F
#define SE_STATE_MACHINE_HASH         0x5EEDA8E9
#define SE_STATE_ACTIONS_HASH         0x25308B8F
#define SE_FIELD_DISPATCH_HASH        0xA1C11B35
#define SE_EVENT_DISPATCH_HASH        0xF3EDFC75
#define SE_DISPATCH_HASH              0xE67DDA18
#define SE_SEQUENCE_HASH              0xEC3EE7BF
#define SE_FORK_HASH                  0x0A24332A
#define SE_FORK_JOIN_HASH             0xE404E1CF
#define SE_CHAIN_FLOW_HASH            0xFFC1FAA4  
#define SE_FOR_HASH                   0xA11A2225
#define SE_WHILE_HASH                 0xA08B6DD3  
#define SE_COND_HASH                  0xCE0831A2
#define SE_VERIFY_AND_CHECK_ELAPSED_TIME_HASH 0x0D3E0C4F
#define SE_VERIFY_AND_CHECK_ELAPSED_EVENTS_HASH 0x1C4FF9A3
#define SE_VERIFY_HASH                      0xC1DED4CF
#define SE_WAIT_HASH                        0xDD7F6663
#define SE_WAIT_TIMEOUT_HASH                0xA16E37EF
#define SE_SEQUENCE_ONCE_HASH               0x4F4BB2E1
#define SE_STACK_FRAME_INSTANCE_HASH        0x753D7572
#define SE_FRAME_ALLOCATE_HASH              0x5C5AC947
#define SE_FRAME_FREE_HASH                  0x633E826E
#define SE_SPAWN_AND_TICK_TREE_HASH         0x00000000
#define SE_EXEC_FN_HASH                     0xC9C09EC0 
#define SE_EXEC_DICT_INTERNAL_HASH          0x2D91A4AC  // SE_EXEC_DICT_INTERNAL
#define SE_EXEC_DICT_DISPATCH_HASH          0xA9BCEF3F  // SE_EXEC_DICT_DISPATCH
#define SE_EXEC_DICT_FN_PTR_HASH            0x0E0A617C
#define SE_SET_EXTERNAL_FIELD_HASH          0x02153947
#define SE_SPAWN_TREE_HASH                  0x756BC19A
#define SE_TICK_TREE_HASH                   0xA56C78DC
// Main functions0xA9BCEF3F  // SE_EXEC_DICT_DISPATCH
// Application result code function hashes (0-5)
#define SE_RETURN_CONTINUE_HASH                0xB4243714
#define SE_RETURN_HALT_HASH                    0x056FB9EA
#define SE_RETURN_TERMINATE_HASH               0xDFE64C74
#define SE_RETURN_RESET_HASH                   0x70EAA030
#define SE_RETURN_DISABLE_HASH                 0x02C11A13
#define SE_RETURN_SKIP_CONTINUE_HASH           0xEAE5524E

// Function result code function hashes (6-11)
#define SE_RETURN_FUNCTION_CONTINUE_HASH       0xF7191B5B
#define SE_RETURN_FUNCTION_HALT_HASH           0x891F0675
#define SE_RETURN_FUNCTION_TERMINATE_HASH      0x0A5B8A85
#define SE_RETURN_FUNCTION_RESET_HASH          0xF6027E85
#define SE_RETURN_FUNCTION_DISABLE_HASH        0x7E3D3A4A
#define SE_RETURN_FUNCTION_SKIP_CONTINUE_HASH  0x338AC3DB

// Pipeline result code function hashes (12-17)
#define SE_RETURN_PIPELINE_CONTINUE_HASH       0x5A902F85
#define SE_RETURN_PIPELINE_HALT_HASH           0x4AC84487
#define SE_RETURN_PIPELINE_TERMINATE_HASH      0x0933AC9B
#define SE_RETURN_PIPELINE_RESET_HASH          0x8CD80263
#define SE_RETURN_PIPELINE_DISABLE_HASH        0x6B7AA500
#define SE_RETURN_PIPELINE_SKIP_CONTINUE_HASH  0xD3A0766D

#define SE_LOG_HASH                   0xCEBBEFA4
#define SE_LOG_INT_HASH               0x2442CEA2

#define SE_LOG_FLOAT_HASH             0xA8949A19
#define SE_LOG_FIELD_HASH             0xBA2925AB
#define SE_SET_HASH_HASH              0xEF5AD4AB

// Field operations (oneshots)
#define SE_SET_FIELD_HASH             0xFFF84A15
#define SE_SET_FIELD_FLOAT_HASH       0x42345454
#define SE_INC_FIELD_HASH             0x09391555
#define SE_DEC_FIELD_HASH             0xC3053EA5
#define SE_LOAD_FUNCTION_DICT_HASH    0xC0225974  // SE_LOAD_FUNCTION_DICT

#define SE_LOAD_FUNCTION_HASH         0x30DB52C3 // SE_LOAD_FUNCTION


// Field comparison predicates
#define SE_FIELD_EQ_HASH              0xD9FBED0D
#define SE_FIELD_NE_HASH              0xF5E4BFD2
#define SE_FIELD_GT_HASH              0x10F63374
#define SE_FIELD_GE_HASH              0x01F61BD7
#define SE_FIELD_LT_HASH              0xD2EA98E7
#define SE_FIELD_LE_HASH              0xC1EA7E24
#define SE_FIELD_IN_RANGE_HASH        0x7BC1968E
#define SE_FIELD_INCREMENT_AND_TEST_HASH 0x275252DB
#define SE_STATE_INCREMENT_AND_TEST_HASH 0x42A88A80
#define SE_LOAD_DICTIONARY_HASH        0x3A543DCB 
#define SE_DICT_EXTRACT_INT_HASH        0xEBB34CA2
#define SE_DICT_EXTRACT_UINT_HASH        0x5C7F2745
#define SE_DICT_EXTRACT_FLOAT_HASH      0x0DC44819
#define SE_DICT_EXTRACT_BOOL_HASH        0x594B60C1
#define SE_DICT_EXTRACT_HASH_HASH        0x93DCE48D
#define SE_DICT_EXTRACT_INT_H_HASH        0x0F34CD9D
#define SE_DICT_EXTRACT_UINT_H_HASH        0x6AFCFC52
#define SE_DICT_EXTRACT_FLOAT_H_HASH        0x6C7F720E
#define SE_DICT_EXTRACT_BOOL_H_HASH        0xF93244F6
#define SE_DICT_EXTRACT_HASH_H_HASH        0x284A5F7A
#define SE_DICT_STORE_PTR_H_HASH 0x136CCC16
#define SE_DICT_STORE_PTR_HASH 0xA5B18AE1
#define SE_PUSH_STACK_HASH 0x08E351ED
#define SE_LOG_STACK_HASH 0x9D0EA7BB
// ============================================================================
// HASH CONSTANTS (FNV-1a 32-bit)
// ============================================================================

#define SE_STACK_ADD_HASH         0x12F0E176U
#define SE_STACK_SUB_HASH         0x60337217U
#define SE_STACK_MUL_HASH         0xF3AFE383U
#define SE_STACK_DIV_HASH         0xE18D8E66U
#define SE_STACK_MOD_HASH         0xDFBECE91U
#define SE_STACK_IDIV_HASH        0xF6AA2F79U
#define SE_STACK_IMOD_HASH        0xEC6554B2U

#define SE_STACK_NEG_HASH         0x9FD819B3U
#define SE_STACK_ABS_HASH         0xD5F4FE9DU
#define SE_STACK_INC_HASH         0x164A29B9U
#define SE_STACK_DEC_HASH         0x2E6FF289U

#define SE_STACK_BAND_HASH        0x5BC94794U
#define SE_STACK_BOR_HASH         0x5829BEA2U
#define SE_STACK_BXOR_HASH        0x2B2EE7A8U
#define SE_STACK_SHL_HASH         0x7013379CU
#define SE_STACK_SHR_HASH         0x661327DEU
#define SE_STACK_SAR_HASH         0x5801C8B3U
#define SE_STACK_BNOT_HASH        0xF8736614U

#define SE_STACK_EQ_HASH          0xF2ADE4E9U
#define SE_STACK_NE_HASH          0xE6BF1B26U
#define SE_STACK_LT_HASH          0x43C42ABBU
#define SE_STACK_LE_HASH          0x32C40FF8U
#define SE_STACK_GT_HASH          0x01A97F58U
#define SE_STACK_GE_HASH          0x12A99A1BU

#define SE_STACK_AND_HASH         0xFEFFCC84U
#define SE_STACK_OR_HASH          0x37BD5C12U
#define SE_STACK_NOT_HASH         0x7CE6ED24U

#define SE_STACK_SQRT_HASH        0x3F4CC355U
#define SE_STACK_EXP_HASH         0xCAD0681AU
#define SE_STACK_LOG_HASH         0x3996A1D3U
#define SE_STACK_LOG10_HASH       0x67B26542U
#define SE_STACK_SIN_HASH         0x1414E55FU
#define SE_STACK_COS_HASH         0xFC51E83AU
#define SE_STACK_TAN_HASH         0x4C4AFE06U
#define SE_STACK_ASIN_HASH        0x216F18E4U
#define SE_STACK_ACOS_HASH        0x338E48C9U
#define SE_STACK_ATAN_HASH        0xDCD3C0D1U
#define SE_STACK_FLOOR_HASH       0x5C254F9FU
#define SE_STACK_CEIL_HASH        0x6AEACFDAU
#define SE_STACK_ROUND_HASH       0x57A023B9U
#define SE_STACK_TRUNC_HASH       0x7AFCEC2BU
#define SE_STACK_POW_HASH         0xA06EFEAFU
#define SE_STACK_ATAN2_HASH       0x8458A559U
#define SE_STACK_MIN_HASH         0x15B91365U
#define SE_STACK_MAX_HASH         0xDFCDD91BU
#define SE_STACK_CLAMP_HASH       0x686343C2U

#define SE_STACK_TOINT_HASH       0x3E489DAFU
#define SE_STACK_TOUINT_HASH      0x33562252U
#define SE_STACK_TOFLOAT_HASH     0x063940F4U

#define SE_STACK_PUSH_CONST_HASH  0x36E29E4BU
#define SE_STACK_ADDI_HASH        0x1032962DU
#define SE_STACK_SUBI_HASH        0xCEFD09FAU
#define SE_STACK_MULI_HASH        0x67E396FEU
#define SE_STACK_DIVI_HASH        0x40D6D3FDU
#define SE_STACK_MODI_HASH        0x115F9E08U
#define SE_STACK_SHLI_HASH        0x4340E44FU
#define SE_STACK_SHRI_HASH        0x472752B5U
#define SE_STACK_SARI_HASH        0x84CF618EU
#define SE_STACK_BANDI_HASH       0x5ADC20E7U
#define SE_STACK_BORI_HASH        0xB4B78BF1U
#define SE_STACK_BXORI_HASH       0xDBD70733U

#define SE_STACK_LOAD_INT_HASH    0xD71F94DDU
#define SE_STACK_LOAD_UINT_HASH   0xF38AC2ACU
#define SE_STACK_LOAD_FLOAT_HASH  0x2820F096U
#define SE_STACK_STORE_INT_HASH   0xDF7950BAU
#define SE_STACK_STORE_UINT_HASH  0xB823630DU
#define SE_STACK_STORE_FLOAT_HASH 0xC80E0EB1U

#define SE_STACK_DROP_HASH        0x6922F6EAU
#define SE_STACK_DROP2_HASH       0x5A0A9608U
#define SE_STACK_DROPN_HASH       0x260A442CU
#define SE_STACK_DUP_HASH         0xDB486070U
#define SE_STACK_DUP2_HASH        0x74EF87E6U
#define SE_STACK_SWAP_HASH        0xDA8EC8C4U
#define SE_STACK_OVER_HASH        0xC26ECF71U
#define SE_STACK_ROT_HASH         0xC4FC4C20U
#define SE_STACK_NROT_HASH        0x5736B5FCU
#define SE_STACK_PICK_HASH        0x9D2B6CE6U
#define SE_STACK_ROLL_HASH        0x8566E52CU

#define SE_STACK_SELECT_HASH      0x58A3D0A3U

#define SE_STACK_PUSH_HASH_HASH   0x9F783538U
#define SE_STACK_HASH_EQ_HASH     0x4DD42786U
#define SE_QUEUE_EVENT_HASH       0xCF729BCEU
#define SE_QUAD_HASH              0x596C457D
#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_BUILTINS_H