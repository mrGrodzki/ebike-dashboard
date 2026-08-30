#include "vesc_can_protocol.h"

#include <string.h>

static uint16_t read_u16_be(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static int16_t read_i16_be(const uint8_t *data)
{
    return (int16_t)read_u16_be(data);
}

static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static int32_t read_i32_be(const uint8_t *data)
{
    return (int32_t)read_u32_be(data);
}

static bool take_i16(const uint8_t *payload, size_t length, size_t *index,
                     float scale, float *value)
{
    if (*index + 2U > length) return false;
    *value = (float)read_i16_be(payload + *index) / scale;
    *index += 2U;
    return true;
}

static bool take_i32(const uint8_t *payload, size_t length, size_t *index,
                     float scale, float *value)
{
    if (*index + 4U > length) return false;
    *value = (float)read_i32_be(payload + *index) / scale;
    *index += 4U;
    return true;
}

static bool take_u32(const uint8_t *payload, size_t length, size_t *index,
                     uint32_t *value)
{
    if (*index + 4U > length) return false;
    *value = read_u32_be(payload + *index);
    *index += 4U;
    return true;
}

static bool take_u8(const uint8_t *payload, size_t length, size_t *index,
                    uint8_t *value)
{
    if (*index + 1U > length) return false;
    *value = payload[*index];
    *index += 1U;
    return true;
}

size_t vesc_can_build_setup_request(uint8_t dashboard_id, uint32_t mask,
                                    uint8_t output[8])
{
    if (output == NULL) return 0U;
    output[0] = dashboard_id;
    output[1] = 0U; /* Process command and return the reply over CAN. */
    output[2] = VESC_COMM_GET_VALUES_SETUP_SELECTIVE;
    output[3] = (uint8_t)(mask >> 24);
    output[4] = (uint8_t)(mask >> 16);
    output[5] = (uint8_t)(mask >> 8);
    output[6] = (uint8_t)mask;
    return 7U;
}

size_t vesc_can_build_decoded_adc_request(uint8_t dashboard_id,
                                          uint8_t output[8])
{
    if (output == NULL) return 0U;
    output[0] = dashboard_id;
    output[1] = 0U; /* Process command and return the reply over CAN. */
    output[2] = VESC_COMM_GET_DECODED_ADC;
    return 3U;
}

bool vesc_can_parse_setup_values(const uint8_t *payload, size_t length,
                                 vesc_can_values_t *values)
{
    if (payload == NULL || values == NULL || length < 5U ||
        payload[0] != VESC_COMM_GET_VALUES_SETUP_SELECTIVE) {
        return false;
    }

    const uint32_t mask = read_u32_be(payload + 1U);
    size_t index = 5U;
    vesc_can_values_t parsed = *values;

#define TAKE_FLOAT16(bit, field, scale) \
    do { if (mask & (bit)) { \
        if (!take_i16(payload, length, &index, (scale), &parsed.field)) return false; \
        parsed.valid_mask |= (bit); \
    } } while (0)
#define TAKE_FLOAT32(bit, field, scale) \
    do { if (mask & (bit)) { \
        if (!take_i32(payload, length, &index, (scale), &parsed.field)) return false; \
        parsed.valid_mask |= (bit); \
    } } while (0)
#define TAKE_BYTE(bit, field) \
    do { if (mask & (bit)) { \
        if (!take_u8(payload, length, &index, &parsed.field)) return false; \
        parsed.valid_mask |= (bit); \
    } } while (0)
#define TAKE_UINT32(bit, field) \
    do { if (mask & (bit)) { \
        if (!take_u32(payload, length, &index, &parsed.field)) return false; \
        parsed.valid_mask |= (bit); \
    } } while (0)

    TAKE_FLOAT16(VESC_VALUE_TEMP_FET, temp_fet_c, 10.0f);
    TAKE_FLOAT16(VESC_VALUE_TEMP_MOTOR, temp_motor_c, 10.0f);
    TAKE_FLOAT32(VESC_VALUE_MOTOR_CURRENT, motor_current_a, 100.0f);
    TAKE_FLOAT32(VESC_VALUE_INPUT_CURRENT, input_current_a, 100.0f);
    TAKE_FLOAT16(VESC_VALUE_DUTY, duty, 1000.0f);
    TAKE_FLOAT32(VESC_VALUE_RPM, rpm, 1.0f);
    TAKE_FLOAT32(VESC_VALUE_SPEED, speed_mps, 1000.0f);
    TAKE_FLOAT16(VESC_VALUE_INPUT_VOLTAGE, input_voltage_v, 10.0f);
    TAKE_FLOAT16(VESC_VALUE_BATTERY_LEVEL, battery_level, 1000.0f);
    TAKE_FLOAT32(VESC_VALUE_AH_USED, amp_hours_used, 10000.0f);
    TAKE_FLOAT32(VESC_VALUE_AH_CHARGED, amp_hours_charged, 10000.0f);
    TAKE_FLOAT32(VESC_VALUE_WH_USED, watt_hours_used, 10000.0f);
    TAKE_FLOAT32(VESC_VALUE_WH_CHARGED, watt_hours_charged, 10000.0f);
    TAKE_FLOAT32(VESC_VALUE_DISTANCE, distance_m, 1000.0f);
    TAKE_FLOAT32(VESC_VALUE_DISTANCE_ABS, distance_abs_m, 1000.0f);
    TAKE_FLOAT32(VESC_VALUE_PID_POSITION, pid_position, 1000000.0f);
    TAKE_BYTE(VESC_VALUE_FAULT, fault_code);
    TAKE_BYTE(VESC_VALUE_CONTROLLER_ID, controller_id);
    TAKE_BYTE(VESC_VALUE_NUM_CONTROLLERS, num_controllers);
    TAKE_FLOAT32(VESC_VALUE_WH_BATTERY_LEFT, watt_hours_battery_left, 1000.0f);
    TAKE_UINT32(VESC_VALUE_ODOMETER, odometer_m);
    TAKE_UINT32(VESC_VALUE_UPTIME, uptime_ms);

#undef TAKE_FLOAT16
#undef TAKE_FLOAT32
#undef TAKE_BYTE
#undef TAKE_UINT32

    /* Reject malformed packets with trailing or missing fields. */
    if (index != length) return false;
    *values = parsed;
    return true;
}

bool vesc_can_parse_decoded_adc(const uint8_t *payload, size_t length,
                                vesc_can_values_t *values)
{
    if (payload == NULL || values == NULL || length != 17U ||
        payload[0] != VESC_COMM_GET_DECODED_ADC) {
        return false;
    }

    size_t index = 1U;
    vesc_can_values_t parsed = *values;
    if (!take_i32(payload, length, &index, 1000000.0f, &parsed.throttle) ||
        !take_i32(payload, length, &index, 1000000.0f, &parsed.throttle_voltage_v) ||
        !take_i32(payload, length, &index, 1000000.0f, &parsed.adc2) ||
        !take_i32(payload, length, &index, 1000000.0f, &parsed.adc2_voltage_v)) {
        return false;
    }

    parsed.valid_mask |= VESC_VALUE_THROTTLE | VESC_VALUE_THROTTLE_VOLTAGE |
                         VESC_VALUE_ADC2 | VESC_VALUE_ADC2_VOLTAGE;
    *values = parsed;
    return true;
}

bool vesc_can_parse_status(uint8_t packet_id, const uint8_t *data, size_t length,
                           vesc_can_values_t *values)
{
    if (data == NULL || values == NULL) return false;

    switch (packet_id) {
    case VESC_CAN_PACKET_STATUS:
        if (length < 8U) return false;
        values->rpm = (float)read_i32_be(data);
        values->motor_current_a = (float)read_i16_be(data + 4U) / 10.0f;
        values->duty = (float)read_i16_be(data + 6U) / 1000.0f;
        values->valid_mask |= VESC_VALUE_RPM | VESC_VALUE_MOTOR_CURRENT | VESC_VALUE_DUTY;
        return true;

    case VESC_CAN_PACKET_STATUS_2:
        if (length < 8U) return false;
        values->amp_hours_used = (float)read_i32_be(data) / 10000.0f;
        values->amp_hours_charged = (float)read_i32_be(data + 4U) / 10000.0f;
        values->valid_mask |= VESC_VALUE_AH_USED | VESC_VALUE_AH_CHARGED;
        return true;

    case VESC_CAN_PACKET_STATUS_3:
        if (length < 8U) return false;
        values->watt_hours_used = (float)read_i32_be(data) / 10000.0f;
        values->watt_hours_charged = (float)read_i32_be(data + 4U) / 10000.0f;
        values->valid_mask |= VESC_VALUE_WH_USED | VESC_VALUE_WH_CHARGED;
        return true;

    case VESC_CAN_PACKET_STATUS_4:
        if (length < 8U) return false;
        values->temp_fet_c = (float)read_i16_be(data) / 10.0f;
        values->temp_motor_c = (float)read_i16_be(data + 2U) / 10.0f;
        values->input_current_a = (float)read_i16_be(data + 4U) / 10.0f;
        values->valid_mask |= VESC_VALUE_TEMP_FET | VESC_VALUE_TEMP_MOTOR |
                              VESC_VALUE_INPUT_CURRENT;
        return true;

    case VESC_CAN_PACKET_STATUS_5:
        if (length < 6U) return false;
        values->tachometer = read_i32_be(data);
        values->input_voltage_v = (float)read_i16_be(data + 4U) / 10.0f;
        values->valid_mask |= VESC_VALUE_INPUT_VOLTAGE;
        return true;

    default:
        return false;
    }
}

uint16_t vesc_can_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0U;
    if (data == NULL) return crc;

    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
