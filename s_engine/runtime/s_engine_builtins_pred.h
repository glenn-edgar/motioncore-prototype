// ============================================================================
// PREDICATE IMPLEMENTATIONS
// ============================================================================

static bool se_pred_and(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            if (!s_expr_invoke_pred(inst, params, i)) {
                return false;
            }
        }
        i = s_expr_skip_param(params, i);
    }
    return true;
}

static bool se_pred_or(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            if (s_expr_invoke_pred(inst, params, i)) {
                return true;
            }
        }
        i = s_expr_skip_param(params, i);
    }
    return false;
}

static bool se_pred_not(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            return !s_expr_invoke_pred(inst, params, i);
        }
        i = s_expr_skip_param(params, i);
    }
    return true;
}

static bool se_pred_nor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    return !se_pred_or(inst, params, param_count, event_type, event_id, event_data);
}

static bool se_pred_nand(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    return !se_pred_and(inst, params, param_count, event_type, event_id, event_data);
}

static bool se_pred_xor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    int true_count = 0;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            if (s_expr_invoke_pred(inst, params, i)) {
                true_count++;
                if (true_count > 1) return false;
            }
        }
        i = s_expr_skip_param(params, i);
    }
    
    return (true_count == 1);
}

static bool se_true(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    return true;
}

static bool se_false(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    return false;
}

static bool se_check_event(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)event_type; (void)event_data;
    
    
    for (uint16_t i = 0; i < param_count; i++) {
        uint8_t opcode = params[i].type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
            if ((uint16_t)params[i].int_val == event_id) {
                
                return true;
            }
        }
    }
    
    return false;
}


// ============================================================================
// FIELD COMPARISON PREDICATES
// ============================================================================

// SE_FIELD_EQ - field equals value
// params: [field_ref] [int]
static bool se_field_eq(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return false;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return false;
    
    int32_t compare_val = (int32_t)params[1].int_val;
    return (*field_ptr == compare_val);
}

// SE_FIELD_NE - field not equals value
static bool se_field_ne(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    return !se_field_eq(inst, params, param_count, event_type, event_id, event_data);
}

// SE_FIELD_GT - field greater than value
static bool se_field_gt(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return false;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return false;
    
    int32_t compare_val = (int32_t)params[1].int_val;
    return (*field_ptr > compare_val);
}

// SE_FIELD_GE - field greater than or equal
static bool se_field_ge(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return false;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return false;
    
    int32_t compare_val = (int32_t)params[1].int_val;
    return (*field_ptr >= compare_val);
}

// SE_FIELD_LT - field less than value
static bool se_field_lt(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return false;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return false;
    
    int32_t compare_val = (int32_t)params[1].int_val;
    return (*field_ptr < compare_val);
}

// SE_FIELD_LE - field less than or equal
static bool se_field_le(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return false;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return false;
    
    int32_t compare_val = (int32_t)params[1].int_val;
    return (*field_ptr <= compare_val);
}

// SE_FIELD_IN_RANGE - field in [min, max] inclusive
// params: [field_ref] [min] [max]
static bool se_field_in_range(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 3) return false;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return false;
    
    int32_t min_val = (int32_t)params[1].int_val;
    int32_t max_val = (int32_t)params[2].int_val;
    
    return (*field_ptr >= min_val && *field_ptr <= max_val);
}

// ============================================================================
// SE_FIELD_INCREMENT_AND_TEST (predicate)
// Param layout: [FIELD slot_ref] [INT or UINT limit]
// On INIT: stores 0 in field, returns true
// On TICK: increments field, returns (field <= limit)
// ============================================================================

static bool se_field_increment_and_test(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    UNUSED(event_type);

    if (param_count < 3) {
        EXCEPTION("SE_FIELD_INCREMENT_AND_TEST: param_count < 2");
        return false;
    }

    // Validate field reference
    if (!S_EXPR_PARAM_IS_FIELD(params[0].type)) {
        EXCEPTION("SE_FIELD_INCREMENT_AND_TEST: params[0] is not a field");
        return false;
    }
    if (!S_EXPR_PARAM_IS_FIELD(params[1].type)) {
        EXCEPTION("SE_FIELD_INCREMENT_AND_TEST: params[1] is not a field");
        return false;
    }
    if (!S_EXPR_PARAM_IS_FIELD(params[2].type)) {
        EXCEPTION("SE_FIELD_INCREMENT_AND_TEST: params[2] is not a field");
        return false;
    }
    if (!inst->blackboard){
        EXCEPTION("SE_FIELD_INCREMENT_AND_TEST: no blackboard bound");
        return false;
    }
    ct_int_t* field = S_EXPR_GET_FIELD(inst, &params[0], ct_int_t);
    ct_int_t *increment_field = S_EXPR_GET_FIELD(inst, &params[1], ct_int_t);
    ct_int_t *limit_field = S_EXPR_GET_FIELD(inst, &params[2], ct_int_t);
    // Get limit from second parameter (INT or UINT)
    
    
   
    
    uint8_t system_flag = s_expr_get_system_flags(inst);
    if (!(system_flag & S_EXPR_NODE_FLAG_INITIALIZED)) {
        s_expr_set_system_flags(inst, system_flag | S_EXPR_NODE_FLAG_INITIALIZED);
        *field = 0;
    
    }
 
    // TICK: increment field and test
    *field += *increment_field;

    return (*field <= *limit_field);
}

// ============================================================================
// SE_STATE_INCREMENT_AND_TEST (predicate)
// Param layout: [INT or UINT limit]
// On INIT: stores 0 in user_data, returns true
// On TICK: increments user_data, returns (user_data <= limit)
// ============================================================================

static bool se_state_increment_and_test(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    UNUSED(event_type);

    if (param_count < 2) {
        EXCEPTION("SE_STATE_INCREMENT_AND_TEST: param_count < 1");
        return false;
    }

    // Get limit from first parameter (INT or UINT)
    uint8_t opcode = params[0].type & S_EXPR_OPCODE_MASK;
    
    uint16_t increment;
    uint16_t limit;

    if (opcode == S_EXPR_PARAM_INT) {
        increment = (uint16_t)params[0].int_val;
    } else if (opcode == S_EXPR_PARAM_UINT) {
        increment = (uint16_t)params[0].uint_val;
    } else {
        EXCEPTION("SE_STATE_INCREMENT_AND_TEST: invalid parameter type");
        return false;
    }

    if (opcode == S_EXPR_PARAM_INT) {
        limit = (uint16_t)params[1].int_val;
    } else if (opcode == S_EXPR_PARAM_UINT) {
        limit = (uint16_t)params[1].uint_val;
    } else {
        EXCEPTION("SE_STATE_INCREMENT_AND_TEST: invalid parameter type");
        return false;
    }

   
    uint8_t system_flag = s_expr_get_system_flags(inst);
    if (!(system_flag & S_EXPR_NODE_FLAG_INITIALIZED)) {
        s_expr_set_system_flags(inst, system_flag | S_EXPR_NODE_FLAG_INITIALIZED);
        s_expr_set_user_flags(inst, 0);
    
    }

    // TICK: increment and test
    uint16_t count = s_expr_get_user_flags(inst);
    count += increment;
    s_expr_set_user_flags(inst, count);
    //printf("SE_STATE_INCREMENT_AND_TEST: TICK: count = %d, increment = %d, limit = %d\n", count, increment, limit);
    return (count <= limit);
}


