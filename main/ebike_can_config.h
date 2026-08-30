#pragma once

/*
 * User-editable CAN/VESC configuration.
 *
 * GPIO17 and GPIO18 are exposed on the Waveshare ESP32-S3-Touch-LCD-2 header
 * and are not used by this project's LCD, touch, IMU, or backlight drivers.
 * They still require an external 3.3 V CAN transceiver.
 */
#define EBIKE_CAN_TX_GPIO                 17
#define EBIKE_CAN_RX_GPIO                 18
#define EBIKE_CAN_BITRATE                 500000

/* Onboard microSD shares MOSI/SCLK with the LCD and has its own chip select. */
#define EBIKE_SD_CS_GPIO                  41
#define EBIKE_LOG_PERIOD_MS               100

/* VESC controller ID from VESC Tool. The dashboard must use a different ID. */
#define EBIKE_VESC_CONTROLLER_ID          0
#define EBIKE_DASHBOARD_CAN_ID            120

#define EBIKE_VESC_POLL_PERIOD_MS         200
#define EBIKE_VESC_ADC_POLL_PERIOD_MS     100
#define EBIKE_VESC_LINK_TIMEOUT_MS        1200

/*
 * The dashboard requests the VESC setup-value packet because it includes
 * already converted speed, battery state, distance, temperature and energy.
 * These bits match COMM_GET_VALUES_SETUP_SELECTIVE in VESC firmware.
 */
#define EBIKE_VESC_SETUP_VALUE_MASK       0x001B79EBUL

/* Used only if the VESC does not return its configured battery percentage. */
#define EBIKE_BATTERY_SERIES_CELLS        13
#define EBIKE_BATTERY_EMPTY_CELL_V        3.20f
#define EBIKE_BATTERY_FULL_CELL_V         4.20f

/*
 * Optional ERPM-to-speed fallback for old VESC firmware. Leave pole pairs at
 * zero to disable it. Prefer configuring wheel diameter/gearing in VESC Tool.
 */
#define EBIKE_VESC_MOTOR_POLE_PAIRS       0
#define EBIKE_VESC_GEAR_RATIO             1.0f
#define EBIKE_WHEEL_DIAMETER_M            0.6985f

/* Estimated consumption used before enough live data exists for range. */
#define EBIKE_DEFAULT_EFFICIENCY_WH_KM    20.0f
