#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vesc_can_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    vesc_can_values_t values;
    uint32_t last_valid_frame_ms;
    uint32_t tx_poll_count;
    uint32_t tx_error_count;
    uint32_t rx_frame_count;
    uint32_t crc_error_count;
    uint32_t malformed_frame_count;
    uint32_t bus_off_count;
    bool linked;
} ebike_can_snapshot_t;

/* Starts ESP32-S3 TWAI, the VESC poller and receive/recovery task. */
esp_err_t ebike_can_start(void);

/* Thread-safe copy of the most recent controller data and link state. */
bool ebike_can_get_snapshot(ebike_can_snapshot_t *snapshot);

/* Requests an immediate setup telemetry packet in addition to periodic polls. */
esp_err_t ebike_can_poll_now(void);

/* Sends a complete VESC command payload using the standard fragmented CAN transport. */
esp_err_t ebike_can_send_vesc_payload(uint8_t sender_id, const uint8_t *payload,
                                      size_t length);

#ifdef __cplusplus
}
#endif
