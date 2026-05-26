// ============================================================================
// samd21_interlock_dsl.c — recursive-descent parser for the interlock DSL.
//
// Grammar (slice 2 — gpio_int subset, locked in design memo
// samd21_interlock_framework_design.md):
//
//   msg       := name (";" section)*
//   section   := keyword "[" content "]"
//   content   := item ("," item)*
//   item      := key (":" value)? | pin_tuple ":" mod_list
//   key       := ident
//   pin_tuple := "(" ident ("," ident)* ")"
//   value     := ident | number
//   mod_list  := ident ("," ident)*
//
//   keywords v1: cfg, watch, out_ok, out_err
//   modes:       in (optional pull modifier: up, down), out
//
// Slice 2 specifically refuses keywords/modes outside this set so the
// parser's failure modes are deterministic. Slice 4 adds ADC modifiers
// (oversample_N, sh_N); slice 6 adds hysteresis (hyst_N) + nested {}.
//
// No heap usage. Caller-supplied il_inst_t is populated in place; on parse
// failure the caller MUST discard *out (left in undefined state).
// ============================================================================

#include "samd21_interlocks.h"
#include "samd21_pin_table.h"
#include "samd21_hal_pin.h"
#include <string.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------

typedef struct {
    const char* text;
    uint16_t    len;
    uint16_t    pos;
} parser_t;

static bool at_end(const parser_t* p) {
    return p->pos >= p->len;
}

static char peek(const parser_t* p) {
    return at_end(p) ? '\0' : p->text[p->pos];
}

static void skip_ws(parser_t* p) {
    while (!at_end(p)) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
        else break;
    }
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || c == '_';
}

static bool is_ident_cont(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Reads an identifier starting at p->pos. Returns 0 if not at an ident start.
// On success, *start = ident first char, *len = length, advances p->pos.
static bool read_ident(parser_t* p, const char** start, uint8_t* len) {
    skip_ws(p);
    if (at_end(p) || !is_ident_start(p->text[p->pos])) return false;
    *start = &p->text[p->pos];
    uint16_t begin = p->pos;
    while (!at_end(p) && is_ident_cont(p->text[p->pos])) p->pos++;
    *len = (uint8_t)(p->pos - begin);
    return true;
}

// Reads a decimal or hex number. *out = value (truncated to u16).
static bool read_number(parser_t* p, uint16_t* out) {
    skip_ws(p);
    if (at_end(p)) return false;
    uint32_t val = 0;
    bool any = false;
    if (p->pos + 1 < p->len
        && p->text[p->pos] == '0'
        && (p->text[p->pos + 1] == 'x' || p->text[p->pos + 1] == 'X')) {
        p->pos += 2;
        while (!at_end(p)) {
            char c = p->text[p->pos];
            uint8_t d = 0;
            if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else break;
            val = (val << 4) | d;
            p->pos++;
            any = true;
        }
    } else {
        while (!at_end(p) && is_digit(p->text[p->pos])) {
            val = val * 10u + (uint32_t)(p->text[p->pos] - '0');
            p->pos++;
            any = true;
        }
    }
    if (!any) return false;
    *out = (uint16_t)val;
    return true;
}

static bool consume_char(parser_t* p, char want) {
    skip_ws(p);
    if (at_end(p) || p->text[p->pos] != want) return false;
    p->pos++;
    return true;
}

static bool ident_equals(const char* s, uint8_t len, const char* lit) {
    if (strlen(lit) != len) return false;
    return memcmp(s, lit, len) == 0;
}

// True if `s[0..len)` is a parameterised modifier of the form `<prefix><digits>`
// where prefix matches `lit_prefix` (with its trailing underscore) and the
// remainder is one or more digits. e.g. is_param_mod("oversample_16",13,"oversample_") → true.
static bool is_param_mod(const char* s, uint8_t len, const char* lit_prefix) {
    size_t plen = strlen(lit_prefix);
    if (len <= plen) return false;
    if (memcmp(s, lit_prefix, plen) != 0) return false;
    for (uint8_t i = (uint8_t)plen; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// Parse the trailing digits of an `<prefix>N` token into a u16. Caller must
// have already verified the token shape via is_param_mod. Returns false on
// overflow past u16.
static bool parse_param_mod_value(const char* s, uint8_t len, const char* lit_prefix,
                                  uint16_t* out) {
    size_t plen = strlen(lit_prefix);
    uint32_t v = 0;
    for (uint8_t i = (uint8_t)plen; i < len; i++) {
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 65535u) return false;
    }
    *out = (uint16_t)v;
    return true;
}

// True for any modifier the parser recognises (used by the lookahead that
// decides whether a comma starts a new group or continues the modifier list).
static bool is_known_modifier(const char* s, uint8_t len) {
    if (ident_equals(s, len, "in"))   return true;
    if (ident_equals(s, len, "out"))  return true;
    if (ident_equals(s, len, "adc"))  return true;
    if (ident_equals(s, len, "up"))   return true;
    if (ident_equals(s, len, "down")) return true;
    if (is_param_mod(s, len, "oversample_")) return true;
    if (is_param_mod(s, len, "sh_"))         return true;
    return false;
}

// Map a SAMPLENUM count (1,2,4,8,16) to its exponent (0..4). Returns 0xFF on invalid.
static uint8_t samplenum_to_exp(uint16_t n) {
    switch (n) {
        case 1:  return 0;
        case 2:  return 1;
        case 4:  return 2;
        case 8:  return 3;
        case 16: return 4;
        default: return 0xFFu;
    }
}

// ---------------------------------------------------------------------------
// Section parsers
// ---------------------------------------------------------------------------

// Locate the il_inst input index for an already-declared pin label.
static int find_input_idx(const il_inst_t* inst, uint8_t phys_id) {
    for (uint8_t i = 0; i < inst->input_count; i++) {
        if (inst->inputs[i].phys_id == phys_id) return (int)i;
    }
    return -1;
}

static int find_output_idx(const il_inst_t* inst, uint8_t phys_id) {
    for (uint8_t i = 0; i < inst->output_count; i++) {
        if (inst->outputs[i].phys_id == phys_id) return (int)i;
    }
    return -1;
}

// cfg[(D1,D2):in,up,(D3):out]  --or--  cfg[D1:in]
// Slice 4 adds: cfg[(A1):adc,oversample_16,sh_8]
// Allows multiple groups separated by commas.
static il_parse_status_t parse_cfg(parser_t* p, il_inst_t* inst) {
    while (true) {
        // Parse pin list: either single ident or "(" ident ("," ident)* ")"
        const char* pin_labels[IL_MAX_INPUTS + IL_MAX_OUTPUTS];
        uint8_t     pin_lens  [IL_MAX_INPUTS + IL_MAX_OUTPUTS];
        uint8_t     n_pins = 0;

        skip_ws(p);
        if (consume_char(p, '(')) {
            while (true) {
                if (n_pins >= IL_MAX_INPUTS + IL_MAX_OUTPUTS) return IL_PARSE_TOO_MANY_INPUTS;
                if (!read_ident(p, &pin_labels[n_pins], &pin_lens[n_pins])) return IL_PARSE_UNEXPECTED_CHAR;
                n_pins++;
                if (consume_char(p, ',')) continue;
                if (!consume_char(p, ')')) return IL_PARSE_UNEXPECTED_CHAR;
                break;
            }
        } else {
            if (!read_ident(p, &pin_labels[0], &pin_lens[0])) return IL_PARSE_UNEXPECTED_CHAR;
            n_pins = 1;
        }
        if (!consume_char(p, ':')) return IL_PARSE_UNEXPECTED_CHAR;

        // Parse mode + optional modifiers. The "list" can hold up to 4 entries:
        // mode + up to 3 qualifiers (e.g. adc + oversample_N + sh_N).
        const char* mod_strs[4];
        uint8_t     mod_lens[4];
        uint8_t     mod_count = 0;
        while (true) {
            if (mod_count >= 4) return IL_PARSE_UNKNOWN_MODE;
            if (!read_ident(p, &mod_strs[mod_count], &mod_lens[mod_count])) return IL_PARSE_UNEXPECTED_CHAR;
            mod_count++;
            if (!consume_char(p, ',')) break;
            // Lookahead: comma followed by '(' means next group begins. An
            // ident that is NOT a known modifier also ends the list.
            skip_ws(p);
            if (peek(p) == '(') break;
            if (!is_ident_start(peek(p))) break;
            uint16_t save = p->pos;
            const char* ns; uint8_t nl;
            if (!read_ident(p, &ns, &nl)) return IL_PARSE_UNEXPECTED_CHAR;
            bool is_mod = is_known_modifier(ns, nl);
            p->pos = save;
            if (!is_mod) break;
        }

        // Resolve mode from first modifier; rest are qualifiers (pull / adc cfg).
        il_pin_mode_t mode;
        bool is_input = false;
        bool is_adc   = false;
        if      (ident_equals(mod_strs[0], mod_lens[0], "in"))  { mode = IL_PIN_MODE_IN;  is_input = true;  }
        else if (ident_equals(mod_strs[0], mod_lens[0], "out")) { mode = IL_PIN_MODE_OUT; is_input = false; }
        else if (ident_equals(mod_strs[0], mod_lens[0], "adc")) { mode = IL_PIN_MODE_ADC; is_input = true; is_adc = true; }
        else return IL_PARSE_UNKNOWN_MODE;

        // ADC default config: 1 sample (exp=0), 4 sample-hold cycles. Matches
        // the bench-validated values in samd21_commands.c adc_init.
        uint8_t adc_oversample_exp = 0;
        uint8_t adc_sh_cyc         = 4;

        for (uint8_t i = 1; i < mod_count; i++) {
            const char* ms = mod_strs[i];
            uint8_t     ml = mod_lens[i];

            if (is_adc) {
                if (is_param_mod(ms, ml, "oversample_")) {
                    uint16_t n;
                    if (!parse_param_mod_value(ms, ml, "oversample_", &n))
                        return IL_PARSE_OVERSAMPLE_OUT_OF_RANGE;
                    uint8_t exp = samplenum_to_exp(n);
                    if (exp == 0xFFu) return IL_PARSE_OVERSAMPLE_OUT_OF_RANGE;
                    adc_oversample_exp = exp;
                } else if (is_param_mod(ms, ml, "sh_")) {
                    uint16_t n;
                    if (!parse_param_mod_value(ms, ml, "sh_", &n))
                        return IL_PARSE_SH_OUT_OF_RANGE;
                    if (n > 63u) return IL_PARSE_SH_OUT_OF_RANGE;
                    adc_sh_cyc = (uint8_t)n;
                } else {
                    return IL_PARSE_UNKNOWN_MODE;
                }
            } else if (is_input) {
                if      (ident_equals(ms, ml, "up"))   mode = IL_PIN_MODE_IN_PU;
                else if (ident_equals(ms, ml, "down")) mode = IL_PIN_MODE_IN_PD;
                else if (is_param_mod(ms, ml, "oversample_") || is_param_mod(ms, ml, "sh_"))
                    return IL_PARSE_MODIFIER_ON_GPIO;
                else return IL_PARSE_UNKNOWN_MODE;
            } else {
                // out doesn't take any modifiers
                if (is_param_mod(ms, ml, "oversample_") || is_param_mod(ms, ml, "sh_"))
                    return IL_PARSE_MODIFIER_ON_GPIO;
                return IL_PARSE_UNKNOWN_MODE;
            }
        }

        // Record each pin in inst->inputs or inst->outputs.
        for (uint8_t i = 0; i < n_pins; i++) {
            const board_pin_t* bp = board_pin_lookup(pin_labels[i], pin_lens[i]);
            if (bp == 0) return IL_PARSE_UNKNOWN_PIN;
            uint8_t phys_id = board_pin_phys_id(bp);

            if (is_input) {
                if (find_input_idx(inst, phys_id) >= 0)  return IL_PARSE_DUPLICATE_PIN;
                if (find_output_idx(inst, phys_id) >= 0) return IL_PARSE_DUPLICATE_PIN;
                if (inst->input_count >= IL_MAX_INPUTS)  return IL_PARSE_TOO_MANY_INPUTS;
                inst->inputs[inst->input_count].phys_id        = phys_id;
                inst->inputs[inst->input_count].mode           = (uint8_t)mode;
                inst->inputs[inst->input_count].oversample_exp = is_adc ? adc_oversample_exp : 0;
                inst->inputs[inst->input_count].sh_cyc         = is_adc ? adc_sh_cyc         : 0;
                inst->input_count++;
            } else {
                if (find_input_idx(inst, phys_id)  >= 0) return IL_PARSE_DUPLICATE_PIN;
                if (find_output_idx(inst, phys_id) >= 0) return IL_PARSE_DUPLICATE_PIN;
                if (inst->output_count >= IL_MAX_OUTPUTS) return IL_PARSE_TOO_MANY_OUTPUTS;
                inst->outputs[inst->output_count].phys_id   = phys_id;
                inst->outputs[inst->output_count].ok_value  = 0;   // populated by out_ok
                inst->outputs[inst->output_count].err_value = 0;
                inst->output_count++;
            }
        }

        // Continue to next group?
        if (!consume_char(p, ',')) break;
    }
    return IL_PARSE_OK;
}

// watch[D1:1,D2:1]               — slice-2 implicit-eq form (still legal)
// watch[A1:gt:512,A1:lt:3000]    — slice-4 3-part form: pin:op:threshold
//   op ∈ {eq, ne, lt, gt, le, ge}
static il_parse_status_t parse_watch(parser_t* p, il_inst_t* inst) {
    while (true) {
        const char* lbl; uint8_t llen;
        if (!read_ident(p, &lbl, &llen)) return IL_PARSE_UNEXPECTED_CHAR;
        if (!consume_char(p, ':')) return IL_PARSE_UNEXPECTED_CHAR;

        // Disambiguate 2-part vs 3-part by peeking the next non-whitespace char.
        // alpha → op token (3-part form); digit → value (2-part implicit-eq).
        skip_ws(p);
        il_compare_op_t op = IL_OP_EQ;
        if (is_ident_start(peek(p))) {
            const char* opstr; uint8_t oplen;
            if (!read_ident(p, &opstr, &oplen)) return IL_PARSE_UNEXPECTED_CHAR;
            if      (ident_equals(opstr, oplen, "eq")) op = IL_OP_EQ;
            else if (ident_equals(opstr, oplen, "ne")) op = IL_OP_NE;
            else if (ident_equals(opstr, oplen, "lt")) op = IL_OP_LT;
            else if (ident_equals(opstr, oplen, "gt")) op = IL_OP_GT;
            else if (ident_equals(opstr, oplen, "le")) op = IL_OP_LE;
            else if (ident_equals(opstr, oplen, "ge")) op = IL_OP_GE;
            else return IL_PARSE_UNKNOWN_OP;
            if (!consume_char(p, ':')) return IL_PARSE_UNEXPECTED_CHAR;
        }
        uint16_t val;
        if (!read_number(p, &val)) return IL_PARSE_BAD_NUMBER;

        const board_pin_t* bp = board_pin_lookup(lbl, llen);
        if (bp == 0) return IL_PARSE_UNKNOWN_PIN;
        uint8_t phys_id = board_pin_phys_id(bp);
        int idx = find_input_idx(inst, phys_id);
        if (idx < 0) return IL_PARSE_WATCH_INPUT_UNDECL;

        if (inst->watch_count >= IL_MAX_WATCHES) return IL_PARSE_TOO_MANY_WATCHES;
        inst->watches[inst->watch_count].input_idx = (uint8_t)idx;
        inst->watches[inst->watch_count].op        = (uint8_t)op;
        inst->watches[inst->watch_count].threshold = val;
        inst->watch_count++;

        if (!consume_char(p, ',')) break;
    }
    return IL_PARSE_OK;
}

// out_ok[D3:0]  or  out_err[D3:1]
// is_ok = true → write into ok_value; false → err_value.
static il_parse_status_t parse_out_block(parser_t* p, il_inst_t* inst, bool is_ok) {
    while (true) {
        const char* lbl; uint8_t llen;
        if (!read_ident(p, &lbl, &llen)) return IL_PARSE_UNEXPECTED_CHAR;
        if (!consume_char(p, ':')) return IL_PARSE_UNEXPECTED_CHAR;
        uint16_t val;
        if (!read_number(p, &val)) return IL_PARSE_BAD_NUMBER;
        if (val > 1u) return IL_PARSE_BAD_NUMBER;   // GPIO-only for slice 2

        const board_pin_t* bp = board_pin_lookup(lbl, llen);
        if (bp == 0) return IL_PARSE_UNKNOWN_PIN;
        uint8_t phys_id = board_pin_phys_id(bp);
        int idx = find_output_idx(inst, phys_id);
        if (idx < 0) return IL_PARSE_OUTPUT_UNDECL;

        if (is_ok) inst->outputs[idx].ok_value  = (uint8_t)val;
        else       inst->outputs[idx].err_value = (uint8_t)val;

        if (!consume_char(p, ',')) break;
    }
    return IL_PARSE_OK;
}

// ---------------------------------------------------------------------------
// Top-level driver
// ---------------------------------------------------------------------------

il_parse_status_t il_parse(const char* text, uint16_t text_len,
                           il_inst_t* out, uint16_t* err_offset) {
    if (text == 0 || text_len == 0 || out == 0) return IL_PARSE_EMPTY;

    memset(out, 0, sizeof(*out));
    parser_t p = { .text = text, .len = text_len, .pos = 0 };

    // Name
    const char* nstart; uint8_t nlen;
    if (!read_ident(&p, &nstart, &nlen)) {
        if (err_offset) *err_offset = p.pos;
        return IL_PARSE_UNEXPECTED_CHAR;
    }
    if (nlen >= IL_NAME_MAX) {
        if (err_offset) *err_offset = p.pos;
        return IL_PARSE_NAME_TOO_LONG;
    }
    memcpy(out->name, nstart, nlen);
    out->name[nlen] = '\0';

    bool seen_out_ok = false;
    bool seen_out_err = false;

    // Sections
    while (consume_char(&p, ';')) {
        const char* kw; uint8_t klen;
        if (!read_ident(&p, &kw, &klen)) {
            if (err_offset) *err_offset = p.pos;
            return IL_PARSE_UNEXPECTED_CHAR;
        }
        if (!consume_char(&p, '[')) {
            if (err_offset) *err_offset = p.pos;
            return IL_PARSE_UNEXPECTED_CHAR;
        }

        il_parse_status_t st;
        if      (ident_equals(kw, klen, "cfg"))     st = parse_cfg(&p, out);
        else if (ident_equals(kw, klen, "watch"))   st = parse_watch(&p, out);
        else if (ident_equals(kw, klen, "out_ok"))  { st = parse_out_block(&p, out, true);  if (st == IL_PARSE_OK) seen_out_ok = true; }
        else if (ident_equals(kw, klen, "out_err")) { st = parse_out_block(&p, out, false); if (st == IL_PARSE_OK) seen_out_err = true; }
        else {
            if (err_offset) *err_offset = p.pos;
            return IL_PARSE_UNKNOWN_KEYWORD;
        }
        if (st != IL_PARSE_OK) {
            if (err_offset) *err_offset = p.pos;
            return st;
        }

        if (!consume_char(&p, ']')) {
            if (err_offset) *err_offset = p.pos;
            return IL_PARSE_UNEXPECTED_CHAR;
        }
    }

    skip_ws(&p);
    if (!at_end(&p)) {
        if (err_offset) *err_offset = p.pos;
        return IL_PARSE_UNEXPECTED_CHAR;
    }

    if (out->output_count > 0u) {
        if (!seen_out_ok)  return IL_PARSE_MISSING_OUT_OK;
        if (!seen_out_err) return IL_PARSE_MISSING_OUT_ERR;
        // ok and err values must differ on each output pin — otherwise the
        // interlock can't actually communicate state via that pin.
        for (uint8_t i = 0; i < out->output_count; i++) {
            if (out->outputs[i].ok_value == out->outputs[i].err_value) {
                return IL_PARSE_OUTPUT_VALUE_MISMATCH;
            }
        }
    }

    out->tf_state = (uint8_t)IL_TF_UNEVALUATED;
    return IL_PARSE_OK;
}
