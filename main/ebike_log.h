#pragma once

#include <stdbool.h>

#include "driver/spi_common.h"
#include "ebike_can.h"
#include "esp_err.h"

#define EBIKE_LOG_MOUNT_POINT "/sdcard"

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the onboard microSD card on an already initialized SPI bus. */
esp_err_t ebike_log_init(spi_host_device_t spi_host, int chip_select_gpio);

/* Appends one CSV sample. Calls are ignored cleanly when no card is mounted. */
void ebike_log_write(const ebike_can_snapshot_t *snapshot);

bool ebike_log_is_ready(void);
const char *ebike_log_filename(void);

/* Flushes the active CSV so a Wi-Fi download includes all completed samples. */
esp_err_t ebike_log_sync(void);

#ifdef __cplusplus
}
#endif
