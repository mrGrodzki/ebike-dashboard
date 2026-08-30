#include "ebike_trip.h"

#include <math.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TRIP_SAVE_INTERVAL_MS 30000U
#define TRIP_MAX_VALID_DELTA_M 100.0f

static const char *TAG = "ebike_trip";
static SemaphoreHandle_t s_mutex;
static nvs_handle_t s_nvs;
static bool s_nvs_open;
static uint64_t s_local_mm[2];
static float s_last_distance_abs_m;
static bool s_have_last_distance;
static bool s_dirty;
static uint32_t s_last_save_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void save_locked(void)
{
    if (!s_nvs_open || !s_dirty) return;
    esp_err_t err = nvs_set_u64(s_nvs, "trip1_mm", s_local_mm[0]);
    if (err == ESP_OK) err = nvs_set_u64(s_nvs, "trip2_mm", s_local_mm[1]);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err == ESP_OK) {
        s_dirty = false;
        s_last_save_ms = now_ms();
    } else {
        ESP_LOGW(TAG, "Trip save failed: %s", esp_err_to_name(err));
    }
}

esp_err_t ebike_trip_init(void)
{
    if (s_mutex != NULL) return ESP_ERR_INVALID_STATE;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable; trips will be RAM-only: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_open("ebike_trip", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Trip namespace unavailable: %s", esp_err_to_name(err));
        return err;
    }
    s_nvs_open = true;
    if (nvs_get_u64(s_nvs, "trip1_mm", &s_local_mm[0]) != ESP_OK) s_local_mm[0] = 0U;
    if (nvs_get_u64(s_nvs, "trip2_mm", &s_local_mm[1]) != ESP_OK) s_local_mm[1] = 0U;
    s_last_save_ms = now_ms();
    ESP_LOGI(TAG, "Loaded trips: %.3f km / %.3f km",
             (double)s_local_mm[0] / 1000000.0, (double)s_local_mm[1] / 1000000.0);
    return ESP_OK;
}

void ebike_trip_update_distance(float distance_abs_m, bool valid)
{
    if (s_mutex == NULL || !isfinite(distance_abs_m)) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    if (!valid) {
        s_have_last_distance = false;
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_have_last_distance) {
        const float delta_m = distance_abs_m - s_last_distance_abs_m;
        if (delta_m >= 0.0f && delta_m <= TRIP_MAX_VALID_DELTA_M) {
            const uint64_t delta_mm = (uint64_t)(delta_m * 1000.0f + 0.5f);
            if (delta_mm > 0U) {
                s_local_mm[0] += delta_mm;
                s_local_mm[1] += delta_mm;
                s_dirty = true;
            }
        }
    }
    s_last_distance_abs_m = distance_abs_m;
    s_have_last_distance = true;

    if (s_dirty && (now_ms() - s_last_save_ms) >= TRIP_SAVE_INTERVAL_MS) save_locked();
    xSemaphoreGive(s_mutex);
}

void ebike_trip_get(float *local_1_km, float *local_2_km)
{
    if (local_1_km != NULL) *local_1_km = 0.0f;
    if (local_2_km != NULL) *local_2_km = 0.0f;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (local_1_km != NULL) *local_1_km = (float)s_local_mm[0] / 1000000.0f;
    if (local_2_km != NULL) *local_2_km = (float)s_local_mm[1] / 1000000.0f;
    xSemaphoreGive(s_mutex);
}

void ebike_trip_reset(unsigned index)
{
    if (index >= 2U || s_mutex == NULL) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_local_mm[index] = 0U;
    s_dirty = true;
    save_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "TRIP %u reset", index + 1U);
}
