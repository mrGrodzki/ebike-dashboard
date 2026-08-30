#include "ebike_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ebike_spi_lock.h"

#define LOG_FLUSH_INTERVAL_MS 5000U

static const char *TAG = "ebike_log";
static sdmmc_card_t *s_card;
static FILE *s_file;
static char s_filename[40];
static uint32_t s_last_flush_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static double log_value(const ebike_can_snapshot_t *snapshot, uint32_t mask,
                        float value)
{
    if (!snapshot->linked || (snapshot->values.valid_mask & mask) == 0U) return NAN;
    return (double)value;
}

esp_err_t ebike_log_init(spi_host_device_t spi_host, int chip_select_gpio)
{
    if (s_file != NULL) return ESP_ERR_INVALID_STATE;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_host;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id = spi_host;
    device.gpio_cs = chip_select_gpio;

    esp_err_t err = esp_vfs_fat_sdspi_mount(EBIKE_LOG_MOUNT_POINT, &host, &device,
                                             &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD not mounted; logging disabled: %s", esp_err_to_name(err));
        return err;
    }

    bool filename_found = false;
    for (unsigned index = 0; index < 1000U; ++index) {
        snprintf(s_filename, sizeof(s_filename), EBIKE_LOG_MOUNT_POINT "/log%03u.csv", index);
        if (access(s_filename, F_OK) != 0) {
            filename_found = true;
            break;
        }
    }
    if (!filename_found) {
        ESP_LOGE(TAG, "No free log000.csv ... log999.csv filename");
        return ESP_ERR_NO_MEM;
    }

    s_file = fopen(s_filename, "w");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "Cannot create %s", s_filename);
        return ESP_FAIL;
    }
    setvbuf(s_file, NULL, _IOFBF, 4096);
    fputs("time_ms,linked,throttle_pct,motor_current_a,input_current_a,duty_cycle_pct,"
          "input_voltage_v,mosfet_temp_c,motor_temp_c,erpm\n", s_file);
    fflush(s_file);
    s_last_flush_ms = now_ms();
    ESP_LOGI(TAG, "VESC CSV log: %s", s_filename);
    return ESP_OK;
}

void ebike_log_write(const ebike_can_snapshot_t *snapshot)
{
    if (s_file == NULL || snapshot == NULL) return;
    if (!ebike_spi_lock_take(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "SPI bus busy; skipped one CSV sample");
        return;
    }
    const vesc_can_values_t *v = &snapshot->values;
    const int result = fprintf(
        s_file,
        "%lu,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.0f\n",
        (unsigned long)now_ms(), snapshot->linked ? 1U : 0U,
        log_value(snapshot, VESC_VALUE_THROTTLE, v->throttle * 100.0f),
        log_value(snapshot, VESC_VALUE_MOTOR_CURRENT, v->motor_current_a),
        log_value(snapshot, VESC_VALUE_INPUT_CURRENT, v->input_current_a),
        log_value(snapshot, VESC_VALUE_DUTY, v->duty * 100.0f),
        log_value(snapshot, VESC_VALUE_INPUT_VOLTAGE, v->input_voltage_v),
        log_value(snapshot, VESC_VALUE_TEMP_FET, v->temp_fet_c),
        log_value(snapshot, VESC_VALUE_TEMP_MOTOR, v->temp_motor_c),
        log_value(snapshot, VESC_VALUE_RPM, v->rpm));

    if (result < 0) {
        ESP_LOGE(TAG, "microSD write failed; logging stopped");
        fclose(s_file);
        s_file = NULL;
        ebike_spi_lock_give();
        return;
    }
    const uint32_t current_ms = now_ms();
    if ((current_ms - s_last_flush_ms) >= LOG_FLUSH_INTERVAL_MS) {
        fflush(s_file);
        s_last_flush_ms = current_ms;
    }
    ebike_spi_lock_give();
}

bool ebike_log_is_ready(void)
{
    return s_file != NULL;
}

const char *ebike_log_filename(void)
{
    return s_file != NULL ? s_filename : NULL;
}

esp_err_t ebike_log_sync(void)
{
    if (s_file == NULL) return ESP_ERR_INVALID_STATE;
    if (!ebike_spi_lock_take(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    const int result = fflush(s_file);
    if (result == 0) s_last_flush_ms = now_ms();
    ebike_spi_lock_give();
    return result == 0 ? ESP_OK : ESP_FAIL;
}
