#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EBIKE_WIFI_SSID "Ebike-Logs"
#define EBIKE_WIFI_PASSWORD "ebike-logs"

typedef enum {
    EBIKE_WIFI_STATUS_CONNECTING = 0,
    EBIKE_WIFI_STATUS_CONNECTED,
    EBIKE_WIFI_STATUS_SETUP_AP_RESTORED,
} ebike_wifi_status_state_t;

typedef struct {
    ebike_wifi_status_state_t state;
    uint8_t disconnect_reason;
    char message[160];
} ebike_wifi_status_t;

typedef void (*ebike_wifi_status_cb_t)(const ebike_wifi_status_t *status, void *user_data);

/* Starts a WPA2 access point and an HTTP server at http://192.168.4.1/. */
esp_err_t ebike_wifi_start(ebike_wifi_status_cb_t callback, void *user_data);

#ifdef __cplusplus
}
#endif
