#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VESC_CAN_PACKET_FILL_RX_BUFFER = 5,
    VESC_CAN_PACKET_FILL_RX_BUFFER_LONG = 6,
    VESC_CAN_PACKET_PROCESS_RX_BUFFER = 7,
    VESC_CAN_PACKET_PROCESS_SHORT_BUFFER = 8,
    VESC_CAN_PACKET_STATUS = 9,
    VESC_CAN_PACKET_STATUS_2 = 14,
    VESC_CAN_PACKET_STATUS_3 = 15,
    VESC_CAN_PACKET_STATUS_4 = 16,
    VESC_CAN_PACKET_STATUS_5 = 27,
};

enum {
    VESC_COMM_GET_VALUES = 4,
    VESC_COMM_GET_VALUES_SELECTIVE = 50,
    VESC_COMM_GET_VALUES_SETUP = 47,
    VESC_COMM_GET_VALUES_SETUP_SELECTIVE = 51,
    VESC_COMM_GET_DECODED_ADC = 32,
};

/* Bit positions used by COMM_GET_VALUES_SETUP_SELECTIVE. */
#define VESC_VALUE_TEMP_FET          (1UL << 0)
#define VESC_VALUE_TEMP_MOTOR        (1UL << 1)
#define VESC_VALUE_MOTOR_CURRENT     (1UL << 2)
#define VESC_VALUE_INPUT_CURRENT     (1UL << 3)
#define VESC_VALUE_DUTY              (1UL << 4)
#define VESC_VALUE_RPM               (1UL << 5)
#define VESC_VALUE_SPEED             (1UL << 6)
#define VESC_VALUE_INPUT_VOLTAGE     (1UL << 7)
#define VESC_VALUE_BATTERY_LEVEL     (1UL << 8)
#define VESC_VALUE_AH_USED           (1UL << 9)
#define VESC_VALUE_AH_CHARGED        (1UL << 10)
#define VESC_VALUE_WH_USED           (1UL << 11)
#define VESC_VALUE_WH_CHARGED        (1UL << 12)
#define VESC_VALUE_DISTANCE          (1UL << 13)
#define VESC_VALUE_DISTANCE_ABS      (1UL << 14)
#define VESC_VALUE_PID_POSITION      (1UL << 15)
#define VESC_VALUE_FAULT             (1UL << 16)
#define VESC_VALUE_CONTROLLER_ID     (1UL << 17)
#define VESC_VALUE_NUM_CONTROLLERS   (1UL << 18)
#define VESC_VALUE_WH_BATTERY_LEFT   (1UL << 19)
#define VESC_VALUE_ODOMETER          (1UL << 20)
#define VESC_VALUE_UPTIME            (1UL << 21)
/* Local dashboard fields, not bits in the setup-selective request mask. */
#define VESC_VALUE_THROTTLE          (1UL << 22)
#define VESC_VALUE_THROTTLE_VOLTAGE  (1UL << 23)
#define VESC_VALUE_ADC2              (1UL << 24)
#define VESC_VALUE_ADC2_VOLTAGE      (1UL << 25)

typedef struct {
    uint32_t valid_mask;
    float temp_fet_c;
    float temp_motor_c;
    float motor_current_a;
    float input_current_a;
    float duty;
    float rpm;
    float speed_mps;
    float input_voltage_v;
    float battery_level;
    float amp_hours_used;
    float amp_hours_charged;
    float watt_hours_used;
    float watt_hours_charged;
    float distance_m;
    float distance_abs_m;
    float pid_position;
    uint8_t fault_code;
    uint8_t controller_id;
    uint8_t num_controllers;
    float watt_hours_battery_left;
    uint32_t odometer_m;
    uint32_t uptime_ms;
    int32_t tachometer;
    float throttle;
    float throttle_voltage_v;
    float adc2;
    float adc2_voltage_v;
} vesc_can_values_t;

/* Builds the seven-byte PROCESS_SHORT_BUFFER payload used to poll a VESC. */
size_t vesc_can_build_setup_request(uint8_t dashboard_id, uint32_t mask,
                                    uint8_t output[8]);

/* Builds a three-byte request for the VESC ADC application's decoded values. */
size_t vesc_can_build_decoded_adc_request(uint8_t dashboard_id,
                                          uint8_t output[8]);

/* Parses a reassembled VESC command response. */
bool vesc_can_parse_setup_values(const uint8_t *payload, size_t length,
                                 vesc_can_values_t *values);

/* Parses throttle/ADC1 and ADC2 decoded values returned by the VESC. */
bool vesc_can_parse_decoded_adc(const uint8_t *payload, size_t length,
                                vesc_can_values_t *values);

/* Parses VESC status broadcasts (packet IDs 9, 14, 15, 16 and 27). */
bool vesc_can_parse_status(uint8_t packet_id, const uint8_t *data, size_t length,
                           vesc_can_values_t *values);

/* CRC-16/XMODEM used by VESC fragmented CAN command packets. */
uint16_t vesc_can_crc16(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
