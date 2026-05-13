// ============================================================================
// tutorial_user_functions.c
// User function implementations for Tutorial
// Implements verify pattern: write value, read back, compare with expected
// ============================================================================


#include <stdio.h>
#include <string.h>
#include <math.h>
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_builtins.h"
#include "cfl_exception.h"
#include "s_engine_node.h"
#include  "s_engine_types.h"
#include "black_board_records.h"
// ============================================================================
// Helper: Report error via module error callback
// ============================================================================

static void report_error(s_expr_tree_instance_t* inst, const char* func, const char* msg) {
    if (inst && inst->module && inst->module->error_fn) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s", func, msg);
        inst->module->error_fn(inst, S_EXPR_ERR_NO_BLACKBOARD, buf);
    }
}

static void report_verify_fail(s_expr_tree_instance_t* inst, const char* func) {
    report_error(inst, func, "verification failed");
}

// ============================================================================
// Helper: Float comparison with epsilon
// ============================================================================

static bool float_eq(float a, float b) {
    return fabsf(a - b) < 1e-6f;
}

static bool double_eq(double a, double b) {
    return fabs(a - b) < 1e-12;
}

// ============================================================================
// BLACKBOARD ACCESS FUNCTIONS
// Direct access via inst->blackboard cast to record type
// Pattern: params[0] = value to write, params[1] = expected value
// ============================================================================

void bb_write_verify_int32(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    ScalarDemo_t* rec = (ScalarDemo_t*)inst->blackboard;
    int32_t write_val = (int32_t)s_expr_param_int(&params[0]);
    int32_t expected = (int32_t)s_expr_param_int(&params[1]);
    
    // Write value
    rec->counter = write_val;
    
    // Verify
    if (rec->counter != expected) {
        report_verify_fail(inst, __func__);
    }
}

void bb_write_verify_uint32(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    ScalarDemo_t* rec = (ScalarDemo_t*)inst->blackboard;
    uint32_t write_val = (uint32_t)s_expr_param_uint(&params[0]);
    uint32_t expected = (uint32_t)s_expr_param_uint(&params[1]);
    
    rec->flags = write_val;
    
    if (rec->flags != expected) {
        report_verify_fail(inst, __func__);
    }
}

void bb_write_verify_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    ScalarDemo_t* rec = (ScalarDemo_t*)inst->blackboard;
    float write_val = (float)s_expr_param_float(&params[0]);
    float expected = (float)s_expr_param_float(&params[1]);
    
    rec->temperature = write_val;
    
    if (!float_eq(rec->temperature, expected)) {
        report_verify_fail(inst, __func__);
    }
}

void bb_write_verify_int64(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    ScalarDemo_t* rec = (ScalarDemo_t*)inst->blackboard;
    int64_t write_val = (int64_t)s_expr_param_int(&params[0]);
    int64_t expected = (int64_t)s_expr_param_int(&params[1]);
    
    rec->timestamp = write_val;
    
    if (rec->timestamp != expected) {
        report_verify_fail(inst, __func__);
    }
}

void bb_write_verify_uint64(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    ScalarDemo_t* rec = (ScalarDemo_t*)inst->blackboard;
    uint64_t write_val = (uint64_t)s_expr_param_uint(&params[0]);
    uint64_t expected = (uint64_t)s_expr_param_uint(&params[1]);
    
    rec->checksum = write_val;
    
    if (rec->checksum != expected) {
        report_verify_fail(inst, __func__);
    }
}

void bb_write_verify_double(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    ScalarDemo_t* rec = (ScalarDemo_t*)inst->blackboard;
    double write_val = (double)s_expr_param_float(&params[0]);
    double expected = (double)s_expr_param_float(&params[1]);
    
    rec->precise_value = write_val;
    
    if (!double_eq(rec->precise_value, expected)) {
        report_verify_fail(inst, __func__);
    }
}

// ============================================================================
// SLOT ACCESS FUNCTIONS
// Uses field_ref() parameter to access fields generically via offset
// Pattern: params[0] = field_ref, params[1] = value, params[2] = expected
// ============================================================================

void slot_write_verify_int32(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    int32_t* field = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    int32_t write_val = (int32_t)s_expr_param_int(&params[1]);
    int32_t expected = (int32_t)s_expr_param_int(&params[2]);
    
    *field = write_val;
    
    if (*field != expected) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_uint32(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    uint32_t* field = S_EXPR_GET_FIELD(inst, &params[0], uint32_t);
    uint32_t write_val = (uint32_t)s_expr_param_uint(&params[1]);
    uint32_t expected = (uint32_t)s_expr_param_uint(&params[2]);
    
    *field = write_val;
    
    if (*field != expected) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    float* field = S_EXPR_GET_FIELD(inst, &params[0], float);
    float write_val = (float)s_expr_param_float(&params[1]);
    float expected = (float)s_expr_param_float(&params[2]);
    
    *field = write_val;
    
    if (!float_eq(*field, expected)) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_int64(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    int64_t* field = S_EXPR_GET_FIELD(inst, &params[0], int64_t);
    int64_t write_val = (int64_t)s_expr_param_int(&params[1]);
    int64_t expected = (int64_t)s_expr_param_int(&params[2]);
    
    *field = write_val;
    
    if (*field != expected) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_uint64(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    uint64_t* field = S_EXPR_GET_FIELD(inst, &params[0], uint64_t);
    uint64_t write_val = (uint64_t)s_expr_param_uint(&params[1]);
    uint64_t expected = (uint64_t)s_expr_param_uint(&params[2]);
    
    *field = write_val;
    
    if (*field != expected) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_double(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    double* field = S_EXPR_GET_FIELD(inst, &params[0], double);
    double write_val = (double)s_expr_param_float(&params[1]);
    double expected = (double)s_expr_param_float(&params[2]);
    
    *field = write_val;
    
    if (!double_eq(*field, expected)) {
        report_verify_fail(inst, __func__);
    }
}

// ============================================================================
// ARRAY ACCESS FUNCTIONS
// ============================================================================

void slot_write_verify_string(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || !inst->module || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    // params[0] = field_ref (CHAR_ARRAY field)
    // params[1] = string to write (STR_IDX)
    // params[2] = expected string (STR_IDX)
    
    char* field = S_EXPR_GET_FIELD(inst, &params[0], char);
    uint16_t field_size = params[0].field_size;
    
    // Get string from string table
    const char* write_str = s_expr_param_string(inst->module->def, &params[1]);
    const char* expected_str = s_expr_param_string(inst->module->def, &params[2]);
    
    if (!write_str || !expected_str) {
        report_error(inst, __func__, "string lookup failed");
        return;
    }
    
    // Write string (with null terminator, respecting field size)
    size_t len = strlen(write_str);
    if (len >= field_size) {
        len = field_size - 1;
    }
    memcpy(field, write_str, len);
    field[len] = '\0';
    
    // Verify
    if (strncmp(field, expected_str, field_size) != 0) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_int32_element(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 4) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    // params[0] = field_ref (INT32_ARRAY field)
    // params[1] = index
    // params[2] = value to write
    // params[3] = expected value
    
    int32_t* array = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    uint16_t array_len = params[0].field_size / sizeof(int32_t);
    
    int32_t index = (int32_t)s_expr_param_int(&params[1]);
    int32_t write_val = (int32_t)s_expr_param_int(&params[2]);
    int32_t expected = (int32_t)s_expr_param_int(&params[3]);
    
    // Bounds check
    if (index < 0 || (uint16_t)index >= array_len) {
        report_error(inst, __func__, "index out of bounds");
        return;
    }
    
    // Write and verify
    array[index] = write_val;
    
    if (array[index] != expected) {
        report_verify_fail(inst, __func__);
    }
}

void slot_write_verify_float32_array(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 9) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    // params[0] = field_ref (FLOAT32_ARRAY field)
    // params[1..4] = values to write (4 floats)
    // params[5..8] = expected values (4 floats)
    
    float* array = S_EXPR_GET_FIELD(inst, &params[0], float);
    uint16_t array_len = params[0].field_size / sizeof(float);
    
    // Write all elements
    for (uint16_t i = 0; i < 4 && i < array_len; i++) {
        array[i] = (float)s_expr_param_float(&params[1 + i]);
    }
    
    // Verify all elements
    for (uint16_t i = 0; i < 4 && i < array_len; i++) {
        float expected = (float)s_expr_param_float(&params[5 + i]);
        if (!float_eq(array[i], expected)) {
            report_verify_fail(inst, __func__);
            return;
        }
    }
}

// ============================================================================
// VERIFY-ONLY FUNCTIONS (read and compare, no write)
// ============================================================================

void slot_verify_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    // params[0] = field_ref
    // params[1] = expected value
    
    float* field = S_EXPR_GET_FIELD(inst, &params[0], float);
    float expected = (float)s_expr_param_float(&params[1]);
    
    if (!float_eq(*field, expected)) {
        report_verify_fail(inst, __func__);
    }
}

// ============================================================================
// FUNCTION TABLE REGISTRATION
// ============================================================================

