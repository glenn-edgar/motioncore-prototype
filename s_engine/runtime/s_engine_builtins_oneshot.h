//
// ============================================================================
// ONESHOT IMPLEMENTATIONS
// ============================================================================

static void se_log(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;

    if (param_count < 1) {
        EXCEPTION("SE_LOG: param_count < 1");
        return;
    }
    
    const char* msg = s_expr_get_string(inst, &params[0]);
    if (!msg){
        EXCEPTION("SE_LOG: msg not found");
        return;
    }
    
    double timestamp = 0.0;
    s_expr_module_t* mod = inst->module;
    if (mod && mod->alloc.get_time) {
        timestamp = mod->alloc.get_time(mod->alloc.ctx);
    }
    
    // M-port divergence: no printf fallback. If no debug_fn is registered,
    // se_log is a silent no-op. Reasons:
    //   * SAMD21 has no stdout — printf pulls in newlib-nano's _malloc_r
    //     which sbrk-fails silently, wastes ~6 KB flash, and the bytes go
    //     nowhere anyway (nosys.specs _write returns -1).
    //   * Future versions may emit a debug packet over the libcomm transport;
    //     register a debug_fn to capture the log line until then.
    //
    // Format: "[<uptime_ms>] <message>". Integer ms avoids newlib-nano's
    // float printf (which needs -u _printf_float, ~3 KB flash). %lu format
    // is uniformly available across all M-port chips.
    if (mod && mod->debug_fn) {
        uint32_t uptime_ms = (uint32_t)(timestamp * 1000.0);
        char buf[256];
        snprintf(buf, sizeof(buf), "[%lu] %s", (unsigned long)uptime_ms, msg);
        mod->debug_fn(inst, buf);
    }
}

// SE_LOG_INT - log message with integer value
// params: [str_ptr] [int]
// ============================================================================
// SE_LOG_INT (oneshot)
// Param layout: [STR_IDX format_string] [FIELD slot_ref]
// Retrieves the field value as ct_int_t and prints using the format string.
// Format string should contain one %d/%ld placeholder.
// ============================================================================

static void se_log_int(
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

    if (param_count < 2) {
        EXCEPTION("SE_LOG_INT: param_count < 2");
        return;
    }

    const s_expr_module_def_t* def = s_expr_tree_get_module_def(inst);
    const char* fmt = s_expr_param_string(def, &params[0]);
    if (!fmt) {
        EXCEPTION("SE_LOG_INT: fmt not found");
        return;
    }

    if (!S_EXPR_PARAM_IS_FIELD(params[1].type)) return;
    if (!inst->blackboard) {
        EXCEPTION("SE_LOG_INT: no blackboard");
        return;
    }

    ct_int_t val = *S_EXPR_GET_FIELD(inst, &params[1], ct_int_t);

    double timestamp = 0.0;
    s_expr_module_t* mod = inst->module;
    if (mod && mod->alloc.get_time) {
        timestamp = mod->alloc.get_time(mod->alloc.ctx);
    }

    // M-port divergence: no printf fallback (see se_log comment above).
    if (mod && mod->debug_fn) {
        uint32_t uptime_ms = (uint32_t)(timestamp * 1000.0);
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[%lu] ", (unsigned long)uptime_ms);
        if (n > 0 && n < (int)sizeof(buf)) {
            snprintf(buf + n, sizeof(buf) - n, fmt, val);
        }
        mod->debug_fn(inst, buf);
    }
}
// ============================================================================
// SE_LOG_FLOAT (oneshot)
// Param layout: [STR_IDX format_string] [FIELD slot_ref]
// Retrieves the field value as ct_float_t and prints using the format string.
// Format string should contain one %f/%e/%g placeholder.
// ============================================================================

static void se_log_float(
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

    if (param_count < 2) {
        EXCEPTION("SE_LOG_FLOAT: param_count < 2");
        return;
    }

    const s_expr_module_def_t* def = s_expr_tree_get_module_def(inst);
    const char* fmt = s_expr_param_string(def, &params[0]);
    if (!fmt) {
        EXCEPTION("SE_LOG_FLOAT: fmt not found");
        return;
    }

    if (!S_EXPR_PARAM_IS_FIELD(params[1].type)) {
        EXCEPTION("SE_LOG_FLOAT: params[1] is not a field");
        return;
    }
    if (!inst->blackboard) {
        EXCEPTION("SE_LOG_FLOAT: no blackboard");
        return;
    }

    ct_float_t val = *S_EXPR_GET_FIELD(inst, &params[1], ct_float_t);

    double timestamp = 0.0;
    s_expr_module_t* mod = inst->module;
    if (mod && mod->alloc.get_time) {
        timestamp = mod->alloc.get_time(mod->alloc.ctx);
    }

    // M-port divergence: no printf fallback (see se_log comment above).
    // NOTE: if `fmt` contains %f/%e/%g, the float printf code is pulled in
    // anyway. se_log_float caller's responsibility — flag in upstream review.
    if (mod && mod->debug_fn) {
        uint32_t uptime_ms = (uint32_t)(timestamp * 1000.0);
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[%lu] ", (unsigned long)uptime_ms);
        if (n > 0 && n < (int)sizeof(buf)) {
            snprintf(buf + n, sizeof(buf) - n, fmt, (double)val);
        }
        mod->debug_fn(inst, buf);
    }
}

// SE_LOG_FIELD - log message with field value
// params: [str_ptr] [field_ref]
static void se_log_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    const char* msg = s_expr_get_string(inst, &params[0]);
    if (!msg) return;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[1], int32_t);
    if (!field_ptr) return;
    
    int32_t val = *field_ptr;
    
    double timestamp = 0.0;
    s_expr_module_t* mod = inst->module;
    if (mod && mod->alloc.get_time) {
        timestamp = mod->alloc.get_time(mod->alloc.ctx);
    }
    
    // M-port divergence: no printf fallback (see se_log comment above).
    if (mod && mod->debug_fn) {
        uint32_t uptime_ms = (uint32_t)(timestamp * 1000.0);
        char buf[256];
        snprintf(buf, sizeof(buf), "[%lu] %s %d",
                 (unsigned long)uptime_ms, msg, val);
        mod->debug_fn(inst, buf);
    }
}

// SE_SET_FIELD - set field to integer value
// params: [field_ref] [int]
static void se_set_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return;
    *field_ptr = (int32_t)params[1].int_val;
}

// SE_SET_FIELD_FLOAT - set field to float value
// params: [field_ref] [float]
static void se_set_field_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    float* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], float);
    if (!field_ptr) return;
    
    *field_ptr = params[1].float_val;
}

// SE_INC_FIELD - increment field by delta
// params: [field_ref] [delta]
static void se_inc_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return;
    
    int32_t delta = (int32_t)params[1].int_val;
    *field_ptr += delta;
}

// SE_DEC_FIELD - decrement field by delta
// params: [field_ref] [delta]
static void se_dec_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return;
    
    int32_t delta = (int32_t)params[1].int_val;
    *field_ptr -= delta;
}


void se_push_stack(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->stack || param_count < 1) return;
    
    const s_expr_param_t* p = &params[0];
    uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
    
    s_expr_param_t val;
    memset(&val, 0, sizeof(val));
    
    switch (opcode) {
        case S_EXPR_PARAM_INT:
        case S_EXPR_PARAM_UINT:
        case S_EXPR_PARAM_FLOAT:
        case S_EXPR_PARAM_STR_HASH:
            // Direct copy - already typed
            val = *p;
            break;
            
        case S_EXPR_PARAM_FIELD:
            if (inst->blackboard) {
                uint8_t* src = (uint8_t*)inst->blackboard + p->field_offset;
                if (p->field_size == 4) {
                    val.type = S_EXPR_PARAM_UINT;
                    val.uint_val = *(uint32_t*)src;
                } else if (p->field_size == 8) {
                    val.type = S_EXPR_PARAM_UINT;
                    val.uint_val = *(ct_uint_t*)src;
                }
            }
            break;
            
        case S_EXPR_PARAM_STACK_TOS: {
            const s_expr_param_t* tos = s_expr_stack_peek_tos(inst->stack, p->stack_offset);
            if (tos) val = *tos;
            break;
        }
        
        case S_EXPR_PARAM_STACK_LOCAL: {
            const s_expr_param_t* local = s_expr_stack_get_local(inst->stack, p->stack_offset);
            if (local) val = *local;
            break;
        }
        
        default:
            return;
    }
    
    s_expr_stack_push(inst->stack, &val);
}


// SE_SET_HASH - set field to precomputed hash value
// params: [field_ref] [u32]
static void se_set_hash(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    uint32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], uint32_t);
    if (!field_ptr) return;
    
    *field_ptr = (uint32_t)params[1].uint_val;
   
}

// In SE_QUEUE_EVENT implementation - store the field offset
static void se_queue_event(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    uint16_t ev_type = (uint16_t)params[0].int_val;
    uint16_t ev_id = (uint16_t)params[1].int_val;
    
    // Get field offset as event_data (cast to void* for storage)
    void* ev_data = NULL;
    if (param_count > 2 && S_EXPR_PARAM_IS_FIELD(params[2].type)) {
        ev_data = (void*)(uintptr_t)params[2].field_offset;
    }
    
    // Queue it
    s_expr_event_push(inst, ev_type, ev_id, ev_data);
}




void se_load_function_dict(
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
    
    if (param_count < 2) {
        EXCEPTION("se_load_function_dict: expected 2 params (field_ref, dict)");
        return;
    }
    if (!inst || !inst->blackboard) {
        EXCEPTION("se_load_function_dict: NULL instance or blackboard");
        return;
    }
    
    const s_expr_param_t* field_param = &params[0];
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) {
        EXCEPTION("se_load_function_dict: param[0] is not a FIELD");
        return;
    }
    
    const s_expr_param_t* dict_param = &params[1];
    opcode = dict_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_DICT) {
        EXCEPTION("se_load_function_dict: param[1] is not OPEN_DICT");
        return;
    }
    
    uint16_t offset = field_param->field_offset;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint64_t* ptr_field = (uint64_t*)(bb + offset);
    
    *ptr_field = (uint64_t)(uintptr_t)dict_param;
}

void se_load_function(
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
    
    if (param_count < 2) {
        EXCEPTION("se_load_function: expected 2 params (field_ref, callable)");
        return;
    }
    if (!inst || !inst->blackboard) {
        EXCEPTION("se_load_function: NULL instance or blackboard");
        return;
    }
    
    const s_expr_param_t* field_param = &params[0];
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) {
        EXCEPTION("se_load_function: param[0] is not a FIELD");
        return;
    }
    
    const s_expr_param_t* fn_param = &params[1];
    opcode = fn_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        EXCEPTION("se_load_function: param[1] is not a callable");
        return;
    }
    
    uint16_t offset = field_param->field_offset;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint64_t* ptr_field = (uint64_t*)(bb + offset);
    
    *ptr_field = (uint64_t)(uintptr_t)fn_param;
}

void se_set_external_field(
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
    
    if (param_count < 3) {
        EXCEPTION("se_set_external_field: expected 3 params (value_field, tree_pointer, dictionary_offset)");
        return;
    }
    if (!inst || !inst->blackboard) {
        EXCEPTION("se_set_external_field: NULL instance or blackboard");
        return;
    }
    
    // param[0]: value_field - uint32 field in local blackboard
    const s_expr_param_t* value_param = &params[0];
    uint8_t opcode = value_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) {
        EXCEPTION("se_set_external_field: param[0] is not a FIELD");
        return;
    }
    
    // param[1]: tree_pointer - ptr64 field holding a tree instance pointer
    const s_expr_param_t* tree_param = &params[1];
    opcode = tree_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) {
        EXCEPTION("se_set_external_field: param[1] is not a FIELD");
        return;
    }
    
    // param[2]: dictionary_offset - uint offset into target blackboard
    const s_expr_param_t* offset_param = &params[2];
    opcode = offset_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_UINT) {
        EXCEPTION("se_set_external_field: param[2] is not a UINT");
        return;
    }
    
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    // Read the uint32 value from local blackboard
    uint32_t value = *(uint32_t*)(bb + value_param->field_offset);
    
    // Read the tree instance pointer from local blackboard
    uint64_t raw_ptr = *(uint64_t*)(bb + tree_param->field_offset);
    s_expr_tree_instance_t* target = (s_expr_tree_instance_t*)(uintptr_t)raw_ptr;
    
    if (!target) {
        EXCEPTION("se_set_external_field: target tree instance is NULL");
        return;
    }
    if (!target->blackboard) {
        EXCEPTION("se_set_external_field: target blackboard is NULL");
        return;
    }
    
    // Write value into target blackboard at dictionary_offset
    uint32_t dict_offset = (uint32_t)offset_param->uint_val;
    uint8_t* target_bb = (uint8_t*)target->blackboard;
    *(uint32_t*)(target_bb + dict_offset) = value;
}