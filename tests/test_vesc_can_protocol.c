#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vesc_can_protocol.h"

static void put_i16(uint8_t *buffer, size_t *index, int16_t value)
{
    buffer[(*index)++] = (uint8_t)((uint16_t)value >> 8);
    buffer[(*index)++] = (uint8_t)value;
}

static void put_i32(uint8_t *buffer, size_t *index, int32_t value)
{
    buffer[(*index)++] = (uint8_t)((uint32_t)value >> 24);
    buffer[(*index)++] = (uint8_t)((uint32_t)value >> 16);
    buffer[(*index)++] = (uint8_t)((uint32_t)value >> 8);
    buffer[(*index)++] = (uint8_t)value;
}

static void put_u32(uint8_t *buffer, size_t *index, uint32_t value)
{
    put_i32(buffer, index, (int32_t)value);
}

static bool near(float actual, float expected)
{
    return fabsf(actual - expected) < 0.001f;
}

static void test_request(void)
{
    uint8_t request[8] = {0};
    size_t length = vesc_can_build_setup_request(120U, 0x001B79EBUL, request);
    const uint8_t expected[] = {120U, 0U, 51U, 0x00U, 0x1BU, 0x79U, 0xEBU};
    assert(length == sizeof(expected));
    assert(memcmp(request, expected, sizeof(expected)) == 0);

    memset(request, 0, sizeof(request));
    length = vesc_can_build_decoded_adc_request(120U, request);
    const uint8_t expected_adc[] = {120U, 0U, VESC_COMM_GET_DECODED_ADC};
    assert(length == sizeof(expected_adc));
    assert(memcmp(request, expected_adc, sizeof(expected_adc)) == 0);
}

static void test_decoded_adc_parser(void)
{
    uint8_t payload[17] = {0};
    size_t i = 0U;
    payload[i++] = VESC_COMM_GET_DECODED_ADC;
    put_i32(payload, &i, 625000);  /* decoded throttle = 0.625 */
    put_i32(payload, &i, 2780000); /* ADC1 = 2.78 V */
    put_i32(payload, &i, 0);       /* decoded ADC2 */
    put_i32(payload, &i, 820000);  /* ADC2 = 0.82 V */

    vesc_can_values_t values = {0};
    assert(vesc_can_parse_decoded_adc(payload, i, &values));
    assert(near(values.throttle, 0.625f));
    assert(near(values.throttle_voltage_v, 2.78f));
    assert(near(values.adc2, 0.0f));
    assert(near(values.adc2_voltage_v, 0.82f));
    assert((values.valid_mask & VESC_VALUE_THROTTLE) != 0U);
    assert(!vesc_can_parse_decoded_adc(payload, i - 1U, &values));
}

static void test_setup_parser(void)
{
    const uint32_t mask = 0x001B79EBUL;
    uint8_t payload[80] = {0};
    size_t i = 0U;
    payload[i++] = VESC_COMM_GET_VALUES_SETUP_SELECTIVE;
    put_u32(payload, &i, mask);
    put_i16(payload, &i, 455);       /* FET 45.5 C */
    put_i16(payload, &i, 612);       /* motor 61.2 C */
    put_i32(payload, &i, 1234);      /* input 12.34 A */
    put_i32(payload, &i, 12500);     /* ERPM */
    put_i32(payload, &i, 9876);      /* 9.876 m/s */
    put_i16(payload, &i, 521);       /* 52.1 V */
    put_i16(payload, &i, 783);       /* 78.3 percent */
    put_i32(payload, &i, 123456);    /* 12.3456 Wh */
    put_i32(payload, &i, 2345);      /* 0.2345 Wh charged */
    put_i32(payload, &i, 18654321);  /* 18654.321 m */
    put_i32(payload, &i, 19543210);  /* 19543.210 m absolute */
    payload[i++] = 0U;               /* no fault */
    payload[i++] = 0U;               /* controller ID */
    put_i32(payload, &i, 456700);    /* 456.7 Wh left */
    put_u32(payload, &i, 54321U);    /* odometer */

    vesc_can_values_t values = {0};
    assert(vesc_can_parse_setup_values(payload, i, &values));
    assert(values.valid_mask == mask);
    assert(near(values.temp_fet_c, 45.5f));
    assert(near(values.temp_motor_c, 61.2f));
    assert(near(values.input_current_a, 12.34f));
    assert(near(values.rpm, 12500.0f));
    assert(near(values.speed_mps, 9.876f));
    assert(near(values.input_voltage_v, 52.1f));
    assert(near(values.battery_level, 0.783f));
    assert(near(values.watt_hours_battery_left, 456.7f));
    assert(near(values.distance_m, 18654.321f));
    assert(near(values.distance_abs_m, 19543.210f));
    assert(values.odometer_m == 54321U);

    assert(!vesc_can_parse_setup_values(payload, i - 1U, &values));
}

static void test_status_parser(void)
{
    const uint8_t status1[] = {0x00, 0x00, 0x27, 0x10, 0x01, 0x2C, 0x01, 0xF4};
    vesc_can_values_t values = {0};
    assert(vesc_can_parse_status(VESC_CAN_PACKET_STATUS, status1,
                                 sizeof(status1), &values));
    assert(near(values.rpm, 10000.0f));
    assert(near(values.motor_current_a, 30.0f));
    assert(near(values.duty, 0.5f));

    const uint8_t status4[] = {0x01, 0xC2, 0x02, 0x58, 0x00, 0x7B, 0x00, 0x00};
    assert(vesc_can_parse_status(VESC_CAN_PACKET_STATUS_4, status4,
                                 sizeof(status4), &values));
    assert(near(values.temp_fet_c, 45.0f));
    assert(near(values.temp_motor_c, 60.0f));
    assert(near(values.input_current_a, 12.3f));
}

static void test_crc(void)
{
    static const uint8_t text[] = "123456789";
    assert(vesc_can_crc16(text, sizeof(text) - 1U) == 0x31C3U);
}

int main(void)
{
    test_request();
    test_setup_parser();
    test_decoded_adc_parser();
    test_status_parser();
    test_crc();
    puts("VESC CAN protocol tests passed");
    return 0;
}
