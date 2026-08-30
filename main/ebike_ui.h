#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EBIKE_MODE_ECO = 0,
    EBIKE_MODE_NORMAL,
    EBIKE_MODE_SPORT,
    EBIKE_MODE_COUNT,
} ebike_ride_mode_t;

typedef enum {
    EBIKE_TRIP_LOCAL_1 = 0,
    EBIKE_TRIP_LOCAL_2,
    EBIKE_TRIP_ODOMETER,
    EBIKE_TRIP_VIEW_COUNT,
} ebike_trip_view_t;

typedef void (*ebike_mode_changed_cb_t)(ebike_ride_mode_t mode, void *user_data);
typedef void (*ebike_trip_reset_cb_t)(ebike_trip_view_t trip, void *user_data);
typedef void (*ebike_ota_install_cb_t)(void *user_data);

typedef struct {
    float speed_kph;
    float efficiency_w_per_km;
    float motor_temp_c;
    float fet_temp_c;
    bool motor_temp_valid;
    bool fet_temp_valid;
    int16_t power_w;
    float input_current_a;

    uint8_t battery_percent;
    float battery_voltage_v;
    uint8_t pas_level;
    bool controller_linked;

    float local_trip_1_km;
    float local_trip_2_km;
    float odometer_km;
    bool odometer_valid;
    uint32_t ride_time_s;
    float estimated_range_km;
} ebike_ui_data_t;

/* Create the dashboard after LVGL and the display driver are initialized. */
void ebike_ui_create(lv_obj_t *parent);
void ebike_ui_destroy(void);
lv_obj_t *ebike_ui_get_root(void);

/* All calls must run in the LVGL task or with the project's LVGL lock held. */
void ebike_ui_update(const ebike_ui_data_t *data);
void ebike_ui_set_speed(float speed_kph);
void ebike_ui_set_efficiency(float w_per_km);
void ebike_ui_set_motor_temperature(int16_t temp_c);
void ebike_ui_set_temperatures(float motor_temp_c, bool motor_valid,
                               float fet_temp_c, bool fet_valid);
void ebike_ui_set_power(int16_t power_w);
void ebike_ui_set_input_current(float current_a);
void ebike_ui_set_battery(uint8_t percent, float voltage_v);
void ebike_ui_set_pas(uint8_t level);
void ebike_ui_set_controller_link(bool linked);
void ebike_ui_set_trip(float trip_km);
void ebike_ui_set_trip_distances(float local_1_km, float local_2_km,
                                 float odometer_km, bool odometer_valid);
ebike_trip_view_t ebike_ui_get_trip_view(void);
void ebike_ui_next_trip_view(void);
void ebike_ui_set_trip_reset_callback(ebike_trip_reset_cb_t callback, void *user_data);
void ebike_ui_set_ride_time(uint32_t seconds);
void ebike_ui_set_range(float range_km);

/* Touch-selectable throttle mode and optional controller integration hook. */
void ebike_ui_set_mode(ebike_ride_mode_t mode);
ebike_ride_mode_t ebike_ui_get_mode(void);
void ebike_ui_set_mode_change_callback(ebike_mode_changed_cb_t callback, void *user_data);
void ebike_ui_next_mode(void);

/* Feed longitudinal acceleration from QMI8658. Range is displayed in m/s^2. */
void ebike_ui_set_acceleration_from_sensor(float acceleration_mps2);

/* Full-screen OTA prompt. All calls require the LVGL lock. */
void ebike_ui_set_ota_install_callback(ebike_ota_install_cb_t callback, void *user_data);
void ebike_ui_show_ota_available(const char *installed_version, const char *available_version,
                                 const char *release_notes);
void ebike_ui_show_ota_progress(uint8_t percent, const char *message);
void ebike_ui_show_ota_message(const char *title, const char *message, bool dismissable);
void ebike_ui_hide_ota(void);
bool ebike_ui_ota_overlay_active(void);

#ifdef __cplusplus
}
#endif
