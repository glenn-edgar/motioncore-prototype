// identity.c — see identity.h.

#include "identity.h"
#include <string.h>

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int identity_parse_register(const uint8_t *payload, uint16_t len, dongle_identity_t *out) {
    if (!payload || !out || len < REGISTER_V2_LEN) return -1;
    if (payload[0] != REGISTER_V2_VER) return -1;

    out->version             = payload[0];
    out->class_id            = rd_u32(&payload[1]);
    out->instance_id         = rd_u32(&payload[5]);
    out->commissioning_state = payload[9];
    memcpy(out->chip_uid, &payload[10], 16);
    out->vid                 = rd_u16(&payload[26]);
    out->pid                 = rd_u16(&payload[28]);
    out->fw_version          = rd_u32(&payload[30]);
    out->build_date          = rd_u32(&payload[34]);
    return 0;
}

dongle_role_t identity_role(uint32_t class_id) {
    switch (class_id) {
        case CLASS_ID_DONGLE:         return ROLE_DONGLE;
        case CLASS_ID_BUS_CONTROLLER: return ROLE_BUS_CONTROLLER;
        case CLASS_ID_SLAVE:          return ROLE_SLAVE;
        default:                      return ROLE_UNKNOWN;
    }
}

const char *identity_role_name(dongle_role_t r) {
    switch (r) {
        case ROLE_DONGLE:         return "dongle";
        case ROLE_BUS_CONTROLLER: return "bus_controller";
        case ROLE_SLAVE:          return "slave";
        default:                  return "unknown";
    }
}
