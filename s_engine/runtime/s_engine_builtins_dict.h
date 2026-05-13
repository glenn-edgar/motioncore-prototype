// ============================================================================
// se_load_dictionary.c
// 
// Oneshot function that stores a pointer to a JSON-style dictionary
// in a 64-bit blackboard field. The dictionary remains in ROM/binary -
// no parsing or allocation needed at runtime.
//
// Parameter format (2 logical parameters):
//   [0] FIELD      - offset/size of the PTR64 field in blackboard
//   [1] OPEN_DICT  - start of dictionary structure (pointer stored)
//
// The stored pointer points directly into the param array, allowing
// zero-copy access to the dictionary structure at runtime.
// ============================================================================

static const s_expr_param_t* get_dict_from_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* field_param
) {
    if (!inst || !inst->blackboard || !field_param) return NULL;
    
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) return NULL;
    
    uint16_t offset = field_param->field_offset;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint64_t* ptr_field = (uint64_t*)(bb + offset);
    
    return (const s_expr_param_t*)(uintptr_t)*ptr_field;
}

// ============================================================================
// HELPER: Get string from module string table
// ============================================================================

static const char* get_string(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* str_param
) {
    if (!inst || !inst->module || !inst->module->def) return NULL;
    
    uint8_t opcode = str_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_STR_IDX) return NULL;
    
    const s_expr_module_def_t* def = inst->module->def;
    if (str_param->str_index >= def->string_count) return NULL;
    
    return def->string_table[str_param->str_index];
}

// ============================================================================
// HELPER: Write integer to blackboard field
// ============================================================================

static void write_int_to_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* field_param,
    ct_int_t value
) {
    if (!inst || !inst->blackboard || !field_param) return;
    
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) return;
    
    uint16_t offset = field_param->field_offset;
    uint16_t size = field_param->field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    switch (size) {
        case 1: *(int8_t*)(bb + offset) = (int8_t)value; break;
        case 2: *(int16_t*)(bb + offset) = (int16_t)value; break;
        case 4: *(int32_t*)(bb + offset) = (int32_t)value; break;
        case 8: *(int64_t*)(bb + offset) = value; break;
        default: break;
    }
}

// ============================================================================
// HELPER: Write unsigned integer to blackboard field
// ============================================================================

static void write_uint_to_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* field_param,
    ct_uint_t value
) {
    if (!inst || !inst->blackboard || !field_param) return;
    
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) return;
    
    uint16_t offset = field_param->field_offset;
    uint16_t size = field_param->field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    switch (size) {
        case 1: *(uint8_t*)(bb + offset) = (uint8_t)value; break;
        case 2: *(uint16_t*)(bb + offset) = (uint16_t)value; break;
        case 4: *(uint32_t*)(bb + offset) = (uint32_t)value; break;
        case 8: *(uint64_t*)(bb + offset) = value; break;
        default: break;
    }
}

// ============================================================================
// HELPER: Write float to blackboard field
// ============================================================================

static void write_float_to_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* field_param,
    ct_float_t value
) {
    if (!inst || !inst->blackboard || !field_param) return;
    
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) return;
    
    uint16_t offset = field_param->field_offset;
    uint16_t size = field_param->field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    if (size == 4) {
        *(float*)(bb + offset) = (float)value;
    } else if (size == 8) {
        *(double*)(bb + offset) = value;
    }
}

// ============================================================================
// HELPER: Write hash to blackboard field
// ============================================================================

static void write_hash_to_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* field_param,
    s_expr_hash_t value
) {
    if (!inst || !inst->blackboard || !field_param) return;
    
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) return;
    
    uint16_t offset = field_param->field_offset;
    uint16_t size = field_param->field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    if (size == 4) {
        *(uint32_t*)(bb + offset) = (uint32_t)value;
    } else if (size == 8) {
        *(uint64_t*)(bb + offset) = value;
    }
}

// ============================================================================
// HELPER: Find destination field (last FIELD param for hash path functions)
// ============================================================================

static uint16_t find_dest_field_index(
    const s_expr_param_t* params,
    uint16_t param_count
) {
    uint16_t dest_idx = param_count - 1;
    while (dest_idx > 0 && (params[dest_idx].type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_FIELD) {
        dest_idx--;
    }
    return dest_idx;
}

// ============================================================================
// HELPER: Collect path hashes from params
// ============================================================================

static uint16_t collect_path_hashes(
    const s_expr_param_t* params,
    uint16_t start_idx,
    uint16_t end_idx,
    s_expr_hash_t* path_hashes,
    uint16_t max_depth
) {
    uint16_t depth = end_idx - start_idx;
    if (depth == 0 || depth > max_depth) return 0;
    
    for (uint16_t i = 0; i < depth; i++) {
        uint8_t opcode = params[start_idx + i].type & S_EXPR_OPCODE_MASK;
        if (opcode != S_EXPR_PARAM_STR_HASH) return 0;
        path_hashes[i] = params[start_idx + i].str_hash;
    }
    
    return depth;
}

// ============================================================================
// SE_LOAD_DICTIONARY
// Params: [0] FIELD (PTR64), [1] OPEN_DICT
// ============================================================================

void se_load_dictionary(
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
    
    if (param_count < 2) return;
    if (!inst || !inst->blackboard) return;
    
    const s_expr_param_t* field_param = &params[0];
    uint8_t opcode = field_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_FIELD) return;
    
    const s_expr_param_t* dict_param = &params[1];
    opcode = dict_param->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_DICT) return;
    
    uint16_t offset = field_param->field_offset;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint64_t* ptr_field = (uint64_t*)(bb + offset);
    
    *ptr_field = (uint64_t)(uintptr_t)dict_param;
}
// ============================================================================
// STRING PATH EXTRACTION
// Params: [0] FIELD (dict ptr), [1] STR_IDX (path), [2] FIELD (dest)
// ============================================================================

void se_dict_extract_int(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    ct_int_t value = se_dicts_get_int(dict, mod_def, path, 0);
    
    write_int_to_field(inst, &params[2], value);
}

void se_dict_extract_uint(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    const char* path = get_string(inst, &params[1]);
    
    // DEBUG - add this
    
    if (dict) {
        printf("  opcode=0x%02X\n", dict->type & 0x3F);
    }
    
    if (!dict) return;
    if (!path) return;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    ct_uint_t value = se_dicts_get_uint(dict, mod_def, path, 0);
    
    write_uint_to_field(inst, &params[2], value);
}
void se_dict_extract_float(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    ct_float_t value = se_dicts_get_float(dict, mod_def, path, 0.0);
    
    write_float_to_field(inst, &params[2], value);
}

void se_dict_extract_bool(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    bool value = se_dicts_get_bool(dict, mod_def, path, false);
    
    write_int_to_field(inst, &params[2], value ? 1 : 0);
}

void se_dict_extract_hash(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    s_expr_hash_t value = se_dicts_get_hash(dict, mod_def, path, 0);
    
    write_hash_to_field(inst, &params[2], value);
}

// ============================================================================
// HASH PATH EXTRACTION
// Params: [0] FIELD (dict ptr), [1..N-1] STR_HASH (path), [N] FIELD (dest)
// ============================================================================

void se_dict_extract_int_h(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    uint16_t dest_idx = find_dest_field_index(params, param_count);
    if (dest_idx <= 1) return;
    
    s_expr_hash_t path_hashes[16];
    uint16_t path_depth = collect_path_hashes(params, 1, dest_idx, path_hashes, 16);
    if (path_depth == 0) return;
    
    ct_int_t value = se_dicth_get_int(dict, path_hashes, path_depth, 0);
    
    write_int_to_field(inst, &params[dest_idx], value);
}

void se_dict_extract_uint_h(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    uint16_t dest_idx = find_dest_field_index(params, param_count);
    if (dest_idx <= 1) return;
    
    s_expr_hash_t path_hashes[16];
    uint16_t path_depth = collect_path_hashes(params, 1, dest_idx, path_hashes, 16);
    if (path_depth == 0) return;
    
    ct_uint_t value = se_dicth_get_uint(dict, path_hashes, path_depth, 0);
    
    write_uint_to_field(inst, &params[dest_idx], value);
}

void se_dict_extract_float_h(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    uint16_t dest_idx = find_dest_field_index(params, param_count);
    if (dest_idx <= 1) return;
    
    s_expr_hash_t path_hashes[16];
    uint16_t path_depth = collect_path_hashes(params, 1, dest_idx, path_hashes, 16);
    if (path_depth == 0) return;
    
    ct_float_t value = se_dicth_get_float(dict, path_hashes, path_depth, 0.0);
    
    write_float_to_field(inst, &params[dest_idx], value);
}

void se_dict_extract_bool_h(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    uint16_t dest_idx = find_dest_field_index(params, param_count);
    if (dest_idx <= 1) return;
    
    s_expr_hash_t path_hashes[16];
    uint16_t path_depth = collect_path_hashes(params, 1, dest_idx, path_hashes, 16);
    if (path_depth == 0) return;
    
    bool value = se_dicth_get_bool(dict, path_hashes, path_depth, false);
    
    write_int_to_field(inst, &params[dest_idx], value ? 1 : 0);
}

void se_dict_extract_hash_h(
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
    
    if (param_count < 3) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    uint16_t dest_idx = find_dest_field_index(params, param_count);
    if (dest_idx <= 1) return;
    
    s_expr_hash_t path_hashes[16];
    uint16_t path_depth = collect_path_hashes(params, 1, dest_idx, path_hashes, 16);
    if (path_depth == 0) return;
    
    // Pass module_def for string lookup
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    s_expr_hash_t value = se_dicth_get_hash(dict, path_hashes, path_depth, mod_def, 0);
    
    write_hash_to_field(inst, &params[dest_idx], value);
}
// ============================================================================
// SE_DICT_STORE_PTR
// Store pointer to sub-dictionary/array at path
// Params: [0] FIELD (dict ptr), [1] STR_IDX (path), [2] FIELD (dest PTR64)
// ============================================================================

void se_dict_store_ptr(
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
    
    if (param_count < 3) return;
    if (!inst || !inst->blackboard) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    se_paths_context_t ctx;
    se_paths_context_init(&ctx);
    
    const s_expr_param_t* sub_elem = se_dicts_resolve(dict, mod_def, path, &ctx);
    
    // DEBUG - add this
    
    if (sub_elem) {
        printf("  opcode=0x%02X brace_idx=%d\n",
               sub_elem->type & 0x3F, sub_elem->brace_idx);
    }
    
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint16_t dest_offset = params[2].field_offset;
    uint64_t* dest_ptr = (uint64_t*)(bb + dest_offset);
    
    *dest_ptr = (uint64_t)(uintptr_t)sub_elem;
}

// ============================================================================
// SE_DICT_STORE_PTR_H
// Store pointer to sub-dictionary/array at hash path
// Params: [0] FIELD (dict ptr), [1..N-1] STR_HASH (path), [N] FIELD (dest PTR64)
// ============================================================================

void se_dict_store_ptr_h(
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
    
    if (param_count < 3) return;
    if (!inst || !inst->blackboard) return;
    
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    // Find destination field (last FIELD param)
    uint16_t dest_idx = find_dest_field_index(params, param_count);
    if (dest_idx <= 1) return;
    
    // Collect path hashes
    s_expr_hash_t path_hashes[16];
    uint16_t path_depth = collect_path_hashes(params, 1, dest_idx, path_hashes, 16);
    if (path_depth == 0) return;
    
    // Resolve path
    se_pathh_context_t ctx;
    se_pathh_context_init(&ctx);
    
    const s_expr_param_t* sub_elem = se_dicth_resolve(dict, path_hashes, path_depth, &ctx);
    
    // Store pointer
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint16_t dest_offset = params[dest_idx].field_offset;
    uint64_t* dest_ptr = (uint64_t*)(bb + dest_offset);
    
    *dest_ptr = (uint64_t)(uintptr_t)sub_elem;
}

