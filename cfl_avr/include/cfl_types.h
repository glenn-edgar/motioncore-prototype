/*
 * cfl_types.h — core type definitions for the cfl_avr engine.
 *
 * Locked design rules:
 *   - Engine is event-driven. INIT/TERMINATE are engine-synthesized targeted
 *     callbacks (not queued). Broadcast events (TIME_TICK, user events) go
 *     through the queue.
 *   - All cross-table references are indexes (uint8_t), never pointers.
 *   - Chains own a CONTIGUOUS range of nodes in node_desc[].
 *   - Per-node param data lives in a shared params_pool[] addressed by offset.
 *   - Runtime mutable state is bit-packed bitmaps + a uint16_t scratch slot.
 *   - The scratch slot is either a value or a pointer (interpreted by kind).
 */
#ifndef CFL_TYPES_H
#define CFL_TYPES_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Return codes from a node's event handler.
 *
 *   CFL_CONTINUE  - node stays active; event flows to next sibling
 *   CFL_HALT      - node stays active; event blocked from later siblings in
 *                   this chain. Other chains still see the event.
 *   CFL_DISABLE   - engine fires TERMINATE on this node, clears its enable
 *                   bit; event continues to next sibling.
 *   CFL_RESET     - engine reset-chains this chain (terminate sweep then
 *                   enable). Event continues to NEXT chain.
 *   CFL_TERMINATE - engine disable-chains this chain (terminate sweep only).
 *                   Event continues to NEXT chain.
 *
 * INIT and TERMINATE event return values are IGNORED. Only the TICK
 * (event-dispatch) path return codes matter.
 * -------------------------------------------------------------------------*/
typedef enum {
    CFL_CONTINUE  = 0,
    CFL_HALT      = 1,
    CFL_DISABLE   = 2,
    CFL_RESET     = 3,
    CFL_TERMINATE = 4
} cfl_status_t;

/* ---------------------------------------------------------------------------
 * Built-in node kinds.
 *
 * The kind byte is the entire byte (no flag bits). Per-node auto_start was
 * dropped: when a chain is enabled, ALL its nodes get armed.
 * -------------------------------------------------------------------------*/
enum {
    CFL_KIND_NOP            = 0,  /* placeholder; ignores all events       */
    CFL_KIND_M_CALL         = 1,  /* call user_fns[fn_idx] for every event */
    CFL_KIND_ONE_SHOT       = 2,  /* INIT: call fn_idx. TICK: DISABLE.     */
    CFL_KIND_TIME_DELAY     = 3,  /* INIT: scratch=0. TIME_TICK:           */
                                  /*   scratch += data; if >= param[0]     */
                                  /*   return DISABLE else HALT.           */
                                  /*   Other events: HALT (blocking).      */
    CFL_KIND_ENABLE_CHAINS  = 4,  /* INIT: enable chain ids param[..count];*/
                                  /*   then DISABLE.                       */
    CFL_KIND_DISABLE_CHAINS = 5,  /* INIT: disable chain ids; then DISABLE.*/
    CFL_KIND_RESET_SELF     = 6,  /* return CFL_RESET on any event         */
    CFL_KIND_TERMINATE_SELF = 10, /* return CFL_TERMINATE on any event —   */
                                  /* canonical way to self-disable a chain  */
    CFL_KIND_WHILE          = 7,  /* poll fn_idx each event;               */
                                  /*   CONTINUE -> HALT, DISABLE -> exit   */
    CFL_KIND_VERIFY         = 8,  /* continuous monitor — every event:     */
                                  /*   fn ok -> CONTINUE, else RESET       */
    CFL_KIND_INIT_TERM      = 9,  /* INIT: fn_idx (acquire);               */
                                  /*   TICK: CONTINUE (passive);           */
                                  /*   TERM: fn_idx_2 (release).           */
    /* User kinds may extend from CFL_KIND_USER upward. */
    CFL_KIND_USER           = 32
};

/* Sentinel for the fn slots meaning "no function attached." */
#define CFL_FN_NONE         0xFFu

/* ---------------------------------------------------------------------------
 * Lifecycle and queued events.
 *
 * INIT/TERMINATE are engine-synthesized targeted callbacks — never queued.
 * TIME_TICK and user events ARE queued (broadcast to all active nodes).
 *
 * Event IDs 0..15 are reserved for the engine. User events start at
 * CFL_EVENT_USER_BASE.
 * -------------------------------------------------------------------------*/
typedef enum {
    CFL_EVENT_INIT      = 0,
    CFL_EVENT_TERMINATE = 1,
    CFL_EVENT_TIME_TICK = 2,
    /* 3..15 reserved */
    CFL_EVENT_USER_BASE = 16
} cfl_event_t;

/* ---------------------------------------------------------------------------
 * Flash-resident descriptors. These live in PROGMEM on AVR.
 * -------------------------------------------------------------------------*/
typedef struct {
    uint8_t  kind;        /* one of CFL_KIND_*                                */
    uint8_t  fn_idx;      /* primary fn (CFL_FN_NONE = none)                  */
    uint8_t  fn_idx_2;    /* secondary fn (INIT_TERM release). CFL_FN_NONE    */
                          /* if unused.                                       */
    uint8_t  param_count; /* number of u16 params at params_pool[param_off]   */
    uint16_t param_off;   /* offset into module->params_pool[]                */
} cfl_node_desc_t;

typedef struct {
    uint8_t  node_first;  /* first node index in module->nodes[]              */
    uint8_t  node_count;  /* contiguous range length                          */
    uint8_t  auto_start;  /* const in DSL — used only by cfl_engine_init      */
    uint8_t  _reserved;   /* keep struct alignment uniform                    */
} cfl_chain_desc_t;

#endif /* CFL_TYPES_H */
