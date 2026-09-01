#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EBIKE_OTA_PRODUCT "ebike-dashboard"
#define EBIKE_OTA_HARDWARE "waveshare-esp32-s3-touch-lcd-2"
#define EBIKE_OTA_CHANNEL "stable"
#define EBIKE_OTA_MANIFEST_URL \
    "https://github.com/mrGrodzki/ebike-dashboard/releases/latest/download/manifest.json"

typedef enum {
    EBIKE_OTA_IDLE = 0,
    EBIKE_OTA_CHECKING,
    EBIKE_OTA_UP_TO_DATE,
    EBIKE_OTA_AVAILABLE,
    EBIKE_OTA_DOWNLOADING,
    EBIKE_OTA_RESTARTING,
    EBIKE_OTA_ERROR,
} ebike_ota_state_t;

typedef struct {
    ebike_ota_state_t state;
    char installed_version[32];
    char available_version[32];
    char release_notes[160];
    char message[128];
    uint32_t image_size;
    uint8_t progress_percent;
    bool network_connected;
} ebike_ota_status_t;

typedef void (*ebike_ota_event_cb_t)(const ebike_ota_status_t *status, void *user_data);

esp_err_t ebike_ota_init(ebike_ota_event_cb_t callback, void *user_data);
esp_err_t ebike_ota_start(void);
void ebike_ota_notify_network(bool connected);
void ebike_ota_request_check(void);
esp_err_t ebike_ota_request_check_and_wait(uint32_t timeout_ms);
esp_err_t ebike_ota_install(void);
void ebike_ota_get_status(ebike_ota_status_t *status);

/* Call only after the display and essential tasks initialized successfully. */
esp_err_t ebike_ota_confirm_running_app(void);

#ifdef __cplusplus
}
#endif
