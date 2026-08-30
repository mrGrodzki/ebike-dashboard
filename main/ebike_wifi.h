#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EBIKE_WIFI_SSID "Ebike-Logs"
#define EBIKE_WIFI_PASSWORD "ebike-logs"

/* Starts a WPA2 access point and an HTTP server at http://192.168.4.1/. */
esp_err_t ebike_wifi_start(void);

#ifdef __cplusplus
}
#endif
