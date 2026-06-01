// identity.h — dongle identity: parse the REGISTER v2 announcement and map a
// class_id to a role. Portable; no transport dependency.
//
// REGISTER v2 payload (38 bytes, little-endian) — see register_dongle
// user_functions.c send_register:
//   [0]       version             = 2
//   [1..4]    class_id            firmware build-time constant
//   [5..8]    instance_id         0 if uncommissioned
//   [9]       commissioning_state 0=UNCOMMISSIONED, 1=COMMISSIONED
//   [10..25]  chip_uid            16-byte factory UID
//   [26..27]  vid                 0x2886
//   [28..29]  pid                 0x802F
//   [30..33]  fw_version          (major<<16)|(minor<<8)|patch
//   [34..37]  build_date          packed YYYYMMDD as u32

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REGISTER_V2_LEN   38u
#define REGISTER_V2_VER    2u

// Role class_ids (interim stubs; MUST track register_dongle user_functions.c
// until kb_build delivers the authoritative class_ids.h catalog).
#define CLASS_ID_DONGLE          0x5E588873u  // motioncore.dongle.register.samd21.v1
#define CLASS_ID_BUS_CONTROLLER  0x5E589000u  // motioncore.bus_controller.samd21.v1
#define CLASS_ID_SLAVE           0x5E589100u  // motioncore.slave.samd21.v1

typedef enum {
    ROLE_UNKNOWN = 0,
    ROLE_DONGLE,
    ROLE_BUS_CONTROLLER,
    ROLE_SLAVE,
} dongle_role_t;

typedef struct {
    uint8_t  version;
    uint32_t class_id;
    uint32_t instance_id;
    uint8_t  commissioning_state;
    uint8_t  chip_uid[16];
    uint16_t vid;
    uint16_t pid;
    uint32_t fw_version;
    uint32_t build_date;
} dongle_identity_t;

// Parse a REGISTER v2 payload into *out. Returns 0 on success, -1 if the payload
// is too short or not version 2.
int identity_parse_register(const uint8_t *payload, uint16_t len, dongle_identity_t *out);

dongle_role_t identity_role(uint32_t class_id);
const char   *identity_role_name(dongle_role_t r);

#ifdef __cplusplus
}
#endif
