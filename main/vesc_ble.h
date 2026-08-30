#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts a VESC Tool-compatible Nordic UART Service BLE peripheral. */
esp_err_t vesc_ble_start(void);

bool vesc_ble_is_connected(void);

#ifdef __cplusplus
}
#endif
