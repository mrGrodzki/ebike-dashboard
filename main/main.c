#include <math.h>
#include <stdio.h>
#include "esp_timer.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_qmi8658.h"

#include "lvgl.h"
#include "ebike_can.h"
#include "ebike_can_config.h"
#include "ebike_log.h"
#include "ebike_ota.h"
#include "ebike_spi_lock.h"
#include "ebike_trip.h"
#include "ebike_ui.h"
#include "ebike_wifi.h"
#include "vesc_ble.h"

#define EXAMPLE_PIN_NUM_SCLK 39
#define EXAMPLE_PIN_NUM_MOSI 38
#define EXAMPLE_PIN_NUM_MISO 40

#define EXAMPLE_SPI_HOST SPI2_HOST

#define EXAMPLE_I2C_NUM 0 // I2C number
#define EXAMPLE_PIN_NUM_I2C_SDA 48
#define EXAMPLE_PIN_NUM_I2C_SCL 47

#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)

#define EXAMPLE_PIN_NUM_LCD_DC 42
#define EXAMPLE_PIN_NUM_LCD_RST -1
#define EXAMPLE_PIN_NUM_LCD_CS 45

#define EXAMPLE_LCD_CMD_BITS 8
#define EXAMPLE_LCD_PARAM_BITS 8

#define EXAMPLE_LCD_H_RES 320
#define EXAMPLE_LCD_V_RES 240
#define EXAMPLE_LCD_DRAW_BUFFER_LINES 20
#define EXAMPLE_LCD_TRANSFER_BYTES \
    (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFFER_LINES * sizeof(lv_color_t))
#define EXAMPLE_TOUCH_NATIVE_H_RES 240
#define EXAMPLE_TOUCH_NATIVE_V_RES 320

#define EXAMPLE_PIN_NUM_BK_LIGHT 1

#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE

#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT // Set duty resolution to 13 bits
#define LCD_BL_LEDC_DUTY (1024)                // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LCD_BL_LEDC_FREQUENCY (10000)          // Frequency in Hertz. Set frequency at 5 kHz

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1

/* Change these values if another PCB edge faces the front of the bicycle. */
#define EBIKE_LONGITUDINAL_ACCEL_AXIS 0 /* 0 = X, 1 = Y, 2 = Z */
#define EBIKE_LONGITUDINAL_ACCEL_SIGN 1.0f

static const char *TAG = "lvgl_example";
static lv_indev_drv_t indev_drv; // Input device driver (Touch)
static lv_disp_drv_t disp_drv;   /*Descriptor of a display driver*/
static SemaphoreHandle_t lvgl_api_mux = NULL;

esp_lcd_panel_handle_t panel_handle;
esp_lcd_touch_handle_t tp;

bool lvgl_lock(int timeout_ms)
{
    // Convert timeout in milliseconds to FreeRTOS ticks
    // If `timeout_ms` is set to -1, the program will block until the condition is met
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_api_mux, timeout_ticks) == pdTRUE;
}

void lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_api_mux);
}

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    lv_disp_flush_ready(&disp_drv);
    return ebike_spi_lock_give_from_isr();
}

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display

    if (!ebike_spi_lock_take(portMAX_DELAY)) {
        lv_disp_flush_ready(drv);
        return;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1,
                                               offsetx2 + 1, offsety2 + 1,
                                               color_map);
    if (err != ESP_OK) {
        ebike_spi_lock_give();
        lv_disp_flush_ready(drv);
    }
}

static bool handle_measured_raw_touch(uint16_t x, uint16_t y)
{
    if (ebike_ui_ota_overlay_active()) return false;
    /* Bottom-left block: large enough for a finger and matches the drawn TRIP. */
    if (x <= 112U && y >= 188U) {
        ebike_ui_next_trip_view();
        ESP_LOGI(TAG, "Raw touch action: TRIP");
        return true;
    }
    /* Measured point was x=160,y=85. This 120x100 box is finger-sized. */
    if (x >= 100U && x <= 220U && y >= 40U && y <= 140U) {
        ebike_ui_next_mode();
        ESP_LOGI(TAG, "Raw touch action: next ride mode");
        return true;
    }
    return false;
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static bool was_pressed;
    static bool raw_action_consumed;
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;
    esp_lcd_touch_read_data(tp);
    /* Get coordinates */
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(tp, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

    if (touchpad_pressed && touchpad_cnt > 0)
    {
        data->point.x = touchpad_x[0] < EXAMPLE_LCD_H_RES
                            ? touchpad_x[0] : EXAMPLE_LCD_H_RES - 1;
        data->point.y = touchpad_y[0] < EXAMPLE_LCD_V_RES
                            ? touchpad_y[0] : EXAMPLE_LCD_V_RES - 1;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!was_pressed) {
            ESP_LOGI(TAG, "Touch: x=%u y=%u", (unsigned)touchpad_x[0],
                     (unsigned)touchpad_y[0]);
            raw_action_consumed = handle_measured_raw_touch(touchpad_x[0],
                                                             touchpad_y[0]);
        }
        was_pressed = true;
        if (raw_action_consumed) data->state = LV_INDEV_STATE_RELEASED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
        was_pressed = false;
        raw_action_consumed = false;
    }
}

void lv_port_disp_init(void)
{
    static lv_disp_draw_buf_t draw_buf;
    lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_TRANSFER_BYTES,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(EXAMPLE_LCD_TRANSFER_BYTES,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(buf2);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2,
                          EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFFER_LINES);

    /*-----------------------------------
     * Register the display in LVGL
     *----------------------------------*/

    lv_disp_drv_init(&disp_drv); /*Basic initialization*/

    /*Set up the functions to access to your display*/

    /*Set the resolution of the display*/
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;

    /*Used to copy the buffer's content to the display*/
    disp_drv.flush_cb = example_lvgl_flush_cb;

    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf;

    /*Required for Example 3)*/
    disp_drv.full_refresh = 0;
    // disp_drv.direct_mode = 1;

    /* Fill a memory array with a color if you have GPU.
     * Note that, in lv_conf.h you can enable GPUs that has built-in support in LVGL.
     * But if you have a different GPU you can use with this callback.*/
    // disp_drv.gpu_fill_cb = gpu_fill;

    /*Finally register the driver*/
    lv_disp_drv_register(&disp_drv);
}

void lv_port_indev_init(void)
{
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    // indev_drv.disp = disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    indev_drv.user_data = tp;

    lv_indev_drv_register(&indev_drv);
}

void display_init(void)
{
    /* Keep the other SPI device deselected while the LCD is initialized. */
    gpio_set_direction(EBIKE_SD_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(EBIKE_SD_CS_GPIO, 1);
    ESP_LOGI(TAG, "SPI BUS init");
    spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_PIN_NUM_SCLK,
        .mosi_io_num = EXAMPLE_PIN_NUM_MOSI,
        .miso_io_num = EXAMPLE_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXAMPLE_LCD_TRANSFER_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EXAMPLE_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "Install panel IO");

    esp_lcd_panel_io_handle_t io_handle = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_LCD_DC,
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .on_color_trans_done = example_notify_lvgl_flush_ready,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EXAMPLE_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    /* The LCD controller is natively 240 x 320. Swap axes for the 320 x 240 bike UI. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
}

void touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;

    ESP_LOGI(TAG, "Initialize I2C");
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EXAMPLE_PIN_NUM_I2C_SDA,
        .scl_io_num = EXAMPLE_PIN_NUM_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    /* Initialize I2C */
    ESP_ERROR_CHECK(i2c_param_config(EXAMPLE_I2C_NUM, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(EXAMPLE_I2C_NUM, i2c_conf.mode, 0, 0, 0));

    ESP_LOGI(TAG, "Initialize touch IO (I2C)");
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)EXAMPLE_I2C_NUM, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        /* Native CST816 coordinates are 240 x 320 and need a landscape rotation. */
        .x_max = EXAMPLE_TOUCH_NATIVE_H_RES,
        .y_max = EXAMPLE_TOUCH_NATIVE_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            /* Mirror native Y, then swap: logical X = 320 - raw Y, logical Y = raw X. */
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller CST816");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp));
}

void bsp_brightness_init(void)
{
    gpio_set_direction(EXAMPLE_PIN_NUM_BK_LIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, 1);

    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQUENCY, // Set output frequency at 5 kHz
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = EXAMPLE_PIN_NUM_BK_LIGHT,
        .duty = 0, // Set duty to 0%
        .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void bsp_brightness_set_level(uint8_t level)
{
    if (level > 100)
    {
        ESP_LOGE(TAG, "Brightness value out of range");
        return;
    }

    uint32_t duty = (level * (LCD_BL_LEDC_DUTY - 1)) / 100;

    ESP_ERROR_CHECK(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL));

    ESP_LOGI(TAG, "LCD brightness set to %d%%", level);
}

void lvgl_tick_timer_init(uint32_t ms)
{
    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"};
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, ms * 1000));
}

static void task(void *param)
{
    // ESP_LOGI(TAG, "run");
    while (1)
    {
        uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        while (1)
        {
            // Lock the mutex due to the LVGL APIs are not thread-safe
            if (lvgl_lock(-1))
            {
                task_delay_ms = lv_timer_handler();
                // Release the mutex
                lvgl_unlock();
            }
            if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
            {
                task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
            }
            else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
            {
                task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
        }
    }
}

static void ride_mode_changed(ebike_ride_mode_t mode, void *user_data)
{
    (void)user_data;
    static const char *names[] = {"ECO", "NORMAL", "SPORT"};
    ESP_LOGI(TAG, "Throttle mode: %s", names[mode]);
    /*
     * This changes the dashboard/throttle profile selection only. Motor torque
     * commands are intentionally not sent by the telemetry firmware.
     */
}

static void trip_reset_requested(ebike_trip_view_t trip, void *user_data)
{
    (void)user_data;
    if (trip == EBIKE_TRIP_LOCAL_1) ebike_trip_reset(0U);
    else if (trip == EBIKE_TRIP_LOCAL_2) ebike_trip_reset(1U);
}

static void ota_install_requested(void *user_data)
{
    (void)user_data;
    esp_err_t err = ebike_ota_install();
    if (err != ESP_OK && lvgl_lock(50)) {
        ebike_ui_show_ota_message("UPDATE BLOCKED", esp_err_to_name(err), true);
        lvgl_unlock();
    }
}

static void ota_status_changed(const ebike_ota_status_t *status, void *user_data)
{
    (void)user_data;
    if (status == NULL || !lvgl_lock(100)) return;
    if (status->state == EBIKE_OTA_AVAILABLE) {
        ebike_ui_show_ota_available(status->installed_version, status->available_version,
                                    status->release_notes);
    } else if (status->state == EBIKE_OTA_DOWNLOADING) {
        ebike_ui_show_ota_progress(status->progress_percent, status->message);
    } else if (status->state == EBIKE_OTA_RESTARTING) {
        ebike_ui_show_ota_message("UPDATE INSTALLED", status->message, false);
    } else if (status->state == EBIKE_OTA_ERROR && ebike_ui_ota_overlay_active()) {
        ebike_ui_show_ota_message("UPDATE ERROR", status->message, true);
    }
    lvgl_unlock();
}

static void wifi_status_changed(const ebike_wifi_status_t *status, void *user_data)
{
    (void)user_data;
    if (status == NULL || status->state != EBIKE_WIFI_STATUS_SETUP_AP_RESTORED ||
        !lvgl_lock(100)) {
        return;
    }
    ebike_ui_show_ota_message("WI-FI SETUP", status->message, true);
    lvgl_unlock();
}

static float select_longitudinal_axis(float x, float y, float z)
{
    if (EBIKE_LONGITUDINAL_ACCEL_AXIS == 1) return y * EBIKE_LONGITUDINAL_ACCEL_SIGN;
    if (EBIKE_LONGITUDINAL_ACCEL_AXIS == 2) return z * EBIKE_LONGITUDINAL_ACCEL_SIGN;
    return x * EBIKE_LONGITUDINAL_ACCEL_SIGN;
}

static void qmi8658_acceleration_task(void *param)
{
    (void)param;
    TickType_t last_wake = xTaskGetTickCount();
    float bias_mps2 = 0.0f;
    float filtered_mps2 = 0.0f;
    uint32_t sample_count = 0;

    while (1) {
        float x, y, z;
        if (bsp_qmi8658_read_acceleration_mps2(&x, &y, &z)) {
            float longitudinal = select_longitudinal_axis(x, y, z);
            if (sample_count == 0U) bias_mps2 = longitudinal;

            /* Fast settling at startup, then slow gravity and mounting-angle compensation. */
            float bias_alpha = sample_count < 40U ? 0.08f : 0.006f;
            bias_mps2 += bias_alpha * (longitudinal - bias_mps2);
            float dynamic_mps2 = longitudinal - bias_mps2;
            filtered_mps2 += 0.18f * (dynamic_mps2 - filtered_mps2);
            sample_count++;

            if (lvgl_lock(20)) {
                ebike_ui_set_acceleration_from_sensor(filtered_mps2);
                lvgl_unlock();
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50));
    }
}

static float clamp_float_local(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static uint8_t battery_percent_from_voltage(float voltage_v)
{
    const float empty_v = EBIKE_BATTERY_SERIES_CELLS * EBIKE_BATTERY_EMPTY_CELL_V;
    const float full_v = EBIKE_BATTERY_SERIES_CELLS * EBIKE_BATTERY_FULL_CELL_V;
    float fraction = (voltage_v - empty_v) / (full_v - empty_v);
    fraction = clamp_float_local(fraction, 0.0f, 1.0f);
    return (uint8_t)(fraction * 100.0f + 0.5f);
}

static float speed_from_erpm(float erpm)
{
#if EBIKE_VESC_MOTOR_POLE_PAIRS > 0
    const float pi = 3.14159265358979323846f;
    const float wheel_rpm = erpm /
        ((float)EBIKE_VESC_MOTOR_POLE_PAIRS * EBIKE_VESC_GEAR_RATIO);
    return fabsf(wheel_rpm) * (pi * EBIKE_WHEEL_DIAMETER_M) * 60.0f / 1000.0f;
#else
    (void)erpm;
    return 0.0f;
#endif
}

static uint8_t pas_for_mode(ebike_ride_mode_t mode)
{
    if (mode == EBIKE_MODE_ECO) return 2U;
    if (mode == EBIKE_MODE_SPORT) return 5U;
    return 3U;
}

static void controller_ui_task(void *param)
{
    (void)param;
    bool controller_seen = false;
    uint8_t previous_fault = 0U;
    uint32_t last_log_ms = 0U;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        ebike_can_snapshot_t snapshot;
        if (!ebike_can_get_snapshot(&snapshot)) {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
            continue;
        }

        float speed_kph = 0.0f;
        float local_trip_1_km = 0.0f;
        float local_trip_2_km = 0.0f;
        if (snapshot.linked) {
            const vesc_can_values_t *v = &snapshot.values;
            if (v->valid_mask & VESC_VALUE_SPEED) {
                speed_kph = fabsf(v->speed_mps) * 3.6f;
            } else if (v->valid_mask & VESC_VALUE_RPM) {
                speed_kph = speed_from_erpm(v->rpm);
            }

            float voltage_v = (v->valid_mask & VESC_VALUE_INPUT_VOLTAGE)
                                  ? v->input_voltage_v : 0.0f;
            float power_w = 0.0f;
            if ((v->valid_mask & VESC_VALUE_INPUT_CURRENT) &&
                (v->valid_mask & VESC_VALUE_INPUT_VOLTAGE)) {
                power_w = v->input_current_a * voltage_v;
            }

            uint8_t battery_percent = 0U;
            if (v->valid_mask & VESC_VALUE_BATTERY_LEVEL) {
                battery_percent = (uint8_t)(clamp_float_local(v->battery_level, 0.0f, 1.0f) *
                                            100.0f + 0.5f);
            } else if (v->valid_mask & VESC_VALUE_INPUT_VOLTAGE) {
                battery_percent = battery_percent_from_voltage(voltage_v);
            }

            float efficiency = -1.0f;
            if (speed_kph > 1.0f && power_w > 0.0f) efficiency = power_w / speed_kph;
            else if (power_w < 0.0f) efficiency = 0.0f;

            float range_km = -1.0f;
            if (v->valid_mask & VESC_VALUE_WH_BATTERY_LEFT) {
                float consumption = efficiency > 5.0f ? efficiency
                                                       : EBIKE_DEFAULT_EFFICIENCY_WH_KM;
                range_km = v->watt_hours_battery_left / consumption;
            }

            const bool distance_abs_valid = (v->valid_mask & VESC_VALUE_DISTANCE_ABS) != 0U;
            ebike_trip_update_distance(v->distance_abs_m, distance_abs_valid);
            ebike_trip_get(&local_trip_1_km, &local_trip_2_km);

            const bool odometer_valid = (v->valid_mask & VESC_VALUE_ODOMETER) != 0U;
            const float odometer_km = odometer_valid ? (float)v->odometer_m / 1000.0f : 0.0f;
            const bool motor_temp_valid = (v->valid_mask & VESC_VALUE_TEMP_MOTOR) != 0U;
            const bool fet_temp_valid = (v->valid_mask & VESC_VALUE_TEMP_FET) != 0U;
            const float input_current_a = (v->valid_mask & VESC_VALUE_INPUT_CURRENT)
                                              ? v->input_current_a : 0.0f;
            int32_t power_rounded = (int32_t)(power_w + (power_w >= 0.0f ? 0.5f : -0.5f));
            if (power_rounded > 9999) power_rounded = 9999;
            if (power_rounded < -999) power_rounded = -999;

            if (lvgl_lock(50)) {
                const ebike_ride_mode_t mode = ebike_ui_get_mode();
                ebike_ui_data_t data = {
                    .speed_kph = speed_kph,
                    .efficiency_w_per_km = efficiency,
                    .motor_temp_c = v->temp_motor_c,
                    .fet_temp_c = v->temp_fet_c,
                    .motor_temp_valid = motor_temp_valid,
                    .fet_temp_valid = fet_temp_valid,
                    .power_w = (int16_t)power_rounded,
                    .input_current_a = input_current_a,
                    .battery_percent = battery_percent,
                    .battery_voltage_v = voltage_v,
                    .pas_level = pas_for_mode(mode),
                    .controller_linked = true,
                    .local_trip_1_km = local_trip_1_km,
                    .local_trip_2_km = local_trip_2_km,
                    .odometer_km = odometer_km,
                    .odometer_valid = odometer_valid,
                    .ride_time_s = 0U,
                    .estimated_range_km = range_km,
                };
                ebike_ui_update(&data);
                lvgl_unlock();
            }

            if ((v->valid_mask & VESC_VALUE_FAULT) && v->fault_code != previous_fault) {
                if (v->fault_code != 0U) {
                    ESP_LOGW(TAG, "VESC fault code: %u", (unsigned)v->fault_code);
                } else if (controller_seen) {
                    ESP_LOGI(TAG, "VESC fault cleared");
                }
                previous_fault = v->fault_code;
            }
            controller_seen = true;
        } else {
            ebike_trip_update_distance(0.0f, false);
            ebike_trip_get(&local_trip_1_km, &local_trip_2_km);
            if (controller_seen && lvgl_lock(50)) {
                /* Keep the last measurements, but make loss of VESC unambiguous. */
                ebike_ui_set_controller_link(false);
                lvgl_unlock();
            }
        }

        const uint32_t current_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (snapshot.linked && (current_ms - last_log_ms) >= EBIKE_LOG_PERIOD_MS) {
            ebike_log_write(&snapshot);
            last_log_ms = current_ms;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    lvgl_api_mux = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(ebike_spi_lock_init() ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t trip_result = ebike_trip_init();
    if (trip_result != ESP_OK) {
        ESP_LOGW(TAG, "Persistent trips unavailable; counters will reset after reboot");
    }
    lv_init();
    display_init();
    (void)ebike_log_init(EXAMPLE_SPI_HOST, EBIKE_SD_CS_GPIO);
    touch_init();
    bsp_qmi8658_init();
    lv_port_disp_init();
    lv_port_indev_init();
    lvgl_tick_timer_init(EXAMPLE_LVGL_TICK_PERIOD_MS);
    bsp_brightness_init();
    bsp_brightness_set_level(80);
    if (lvgl_lock(-1))
    {
        ebike_ui_create(lv_scr_act());
        ebike_ui_set_mode_change_callback(ride_mode_changed, NULL);
        ebike_ui_set_trip_reset_callback(trip_reset_requested, NULL);
        ebike_ui_set_ota_install_callback(ota_install_requested, NULL);
        float local_trip_1_km = 0.0f;
        float local_trip_2_km = 0.0f;
        ebike_trip_get(&local_trip_1_km, &local_trip_2_km);
        ebike_ui_set_trip_distances(local_trip_1_km, local_trip_2_km, 0.0f, false);
        lvgl_unlock();
    }
    xTaskCreatePinnedToCore(task, "bsp_lv_port_task", 1024 * 20, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(qmi8658_acceleration_task, "qmi8658_accel", 4096, NULL, 4, NULL, 0);
    esp_err_t ota_result = ebike_ota_init(ota_status_changed, NULL);
    if (ota_result != ESP_OK) {
        ESP_LOGW(TAG, "OTA initialization failed: %s", esp_err_to_name(ota_result));
    }
    esp_err_t can_result = ebike_can_start();
    if (can_result == ESP_OK) {
        xTaskCreatePinnedToCore(controller_ui_task, "controller_ui", 5120, NULL, 5, NULL, 0);
    } else {
        ESP_LOGE(TAG, "CAN initialization failed: %s", esp_err_to_name(can_result));
        ESP_LOGW(TAG, "Dashboard will show NO VESC");
    }
    esp_err_t wifi_result = ebike_wifi_start(wifi_status_changed, NULL);
    if (wifi_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi log server unavailable: %s", esp_err_to_name(wifi_result));
    }
    if (ota_result == ESP_OK) {
        ota_result = ebike_ota_start();
        if (ota_result != ESP_OK) {
            ESP_LOGW(TAG, "OTA task unavailable: %s", esp_err_to_name(ota_result));
        }
    }
    if (can_result == ESP_OK) {
        esp_err_t ble_result = vesc_ble_start();
        if (ble_result != ESP_OK) {
            ESP_LOGW(TAG, "VESC Tool BLE bridge unavailable: %s",
                     esp_err_to_name(ble_result));
        }
    }
    if (ota_result == ESP_OK) {
        esp_err_t confirm_result = ebike_ota_confirm_running_app();
        if (confirm_result != ESP_OK) {
            ESP_LOGW(TAG, "Could not confirm OTA image: %s", esp_err_to_name(confirm_result));
        }
    }
}
