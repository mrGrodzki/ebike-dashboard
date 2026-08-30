#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VESC_TOOL_MAX_PAYLOAD 512U
#define VESC_TOOL_BRIDGE_CAN_ID 253U

typedef void (*vesc_tool_reply_fn)(const uint8_t *payload, size_t length,
                                   void *user_data);

/* Sends one unframed VESC command payload to the configured ESC over CAN. */
esp_err_t vesc_tool_bridge_send(const uint8_t *payload, size_t length);

/* Receives complete unframed replies reassembled by the CAN task. */
void vesc_tool_bridge_set_reply_callback(vesc_tool_reply_fn callback,
                                         void *user_data);

/* Called internally by ebike_can when a complete bridge reply arrives. */
void vesc_tool_bridge_deliver_reply(const uint8_t *payload, size_t length);

#ifdef __cplusplus
}
#endif
