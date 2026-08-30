#include "ebike_can.h"

#include <string.h>

#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ebike_can_config.h"
#include "vesc_tool_bridge.h"

#define VESC_RX_BUFFER_SIZE 256U

static const char *TAG = "ebike_can";
static SemaphoreHandle_t s_snapshot_mutex;
static ebike_can_snapshot_t s_snapshot;
static TaskHandle_t s_can_task;
static bool s_driver_started;
static uint8_t s_rx_buffer[VESC_RX_BUFFER_SIZE];
static uint8_t s_bridge_rx_buffer[VESC_TOOL_MAX_PAYLOAD];

static esp_err_t transmit_extended(uint32_t identifier, const uint8_t *data,
                                   size_t length)
{
    if (length > 8U) return ESP_ERR_INVALID_SIZE;
    twai_message_t message = {
        .identifier = identifier,
        .data_length_code = (uint8_t)length,
        .extd = 1,
        .rtr = 0,
    };
    if (length > 0U) memcpy(message.data, data, length);
    return twai_transmit(&message, pdMS_TO_TICKS(100));
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void snapshot_note_malformed(void)
{
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_snapshot.malformed_frame_count++;
        xSemaphoreGive(s_snapshot_mutex);
    }
}

static void snapshot_note_crc_error(void)
{
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_snapshot.crc_error_count++;
        xSemaphoreGive(s_snapshot_mutex);
    }
}

static void snapshot_accept_values(const vesc_can_values_t *values)
{
    bool link_restored = false;
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        link_restored = !s_snapshot.linked;
        s_snapshot.values = *values;
        s_snapshot.last_valid_frame_ms = now_ms();
        s_snapshot.rx_frame_count++;
        s_snapshot.linked = true;
        xSemaphoreGive(s_snapshot_mutex);
    }
    if (link_restored) {
        ESP_LOGI(TAG, "Valid VESC telemetry received; CAN link is up");
    }
}

static bool snapshot_copy_values(vesc_can_values_t *values)
{
    bool copied = false;
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        *values = s_snapshot.values;
        copied = true;
        xSemaphoreGive(s_snapshot_mutex);
    }
    return copied;
}

static esp_err_t transmit_command(const uint8_t *payload, size_t length)
{
    const esp_err_t result = transmit_extended(
        EBIKE_VESC_CONTROLLER_ID |
            ((uint32_t)VESC_CAN_PACKET_PROCESS_SHORT_BUFFER << 8),
        payload, length);

    if (s_snapshot_mutex != NULL &&
        xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (result == ESP_OK) s_snapshot.tx_poll_count++;
        else s_snapshot.tx_error_count++;
        xSemaphoreGive(s_snapshot_mutex);
    }
    return result;
}

esp_err_t ebike_can_send_vesc_payload(uint8_t sender_id, const uint8_t *payload,
                                      size_t length)
{
    if (!s_driver_started) return ESP_ERR_INVALID_STATE;
    if (payload == NULL || length == 0U || length > VESC_TOOL_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[8];
    if (length <= 6U) {
        frame[0] = sender_id;
        frame[1] = 0U;
        memcpy(frame + 2U, payload, length);
        return transmit_extended(
            EBIKE_VESC_CONTROLLER_ID |
                ((uint32_t)VESC_CAN_PACKET_PROCESS_SHORT_BUFFER << 8),
            frame, length + 2U);
    }

    size_t offset = 0U;
    while (offset < length && offset <= 255U) {
        const size_t bytes = (length - offset) < 7U ? (length - offset) : 7U;
        frame[0] = (uint8_t)offset;
        memcpy(frame + 1U, payload + offset, bytes);
        esp_err_t err = transmit_extended(
            EBIKE_VESC_CONTROLLER_ID |
                ((uint32_t)VESC_CAN_PACKET_FILL_RX_BUFFER << 8),
            frame, bytes + 1U);
        if (err != ESP_OK) return err;
        offset += bytes;
    }
    while (offset < length) {
        const size_t bytes = (length - offset) < 6U ? (length - offset) : 6U;
        frame[0] = (uint8_t)(offset >> 8);
        frame[1] = (uint8_t)offset;
        memcpy(frame + 2U, payload + offset, bytes);
        esp_err_t err = transmit_extended(
            EBIKE_VESC_CONTROLLER_ID |
                ((uint32_t)VESC_CAN_PACKET_FILL_RX_BUFFER_LONG << 8),
            frame, bytes + 2U);
        if (err != ESP_OK) return err;
        offset += bytes;
    }

    const uint16_t crc = vesc_can_crc16(payload, length);
    frame[0] = sender_id;
    frame[1] = 0U;
    frame[2] = (uint8_t)(length >> 8);
    frame[3] = (uint8_t)length;
    frame[4] = (uint8_t)(crc >> 8);
    frame[5] = (uint8_t)crc;
    return transmit_extended(
        EBIKE_VESC_CONTROLLER_ID |
            ((uint32_t)VESC_CAN_PACKET_PROCESS_RX_BUFFER << 8),
        frame, 6U);
}

static esp_err_t transmit_setup_poll(void)
{
    uint8_t payload[8];
    const size_t length = vesc_can_build_setup_request(
        EBIKE_DASHBOARD_CAN_ID, EBIKE_VESC_SETUP_VALUE_MASK, payload);
    return transmit_command(payload, length);
}

static esp_err_t transmit_adc_poll(void)
{
    uint8_t payload[8];
    const size_t length = vesc_can_build_decoded_adc_request(
        EBIKE_DASHBOARD_CAN_ID, payload);
    return transmit_command(payload, length);
}

esp_err_t ebike_can_poll_now(void)
{
    if (!s_driver_started) return ESP_ERR_INVALID_STATE;
    return transmit_setup_poll();
}

static bool parse_command_response(const uint8_t *payload, size_t length,
                                   vesc_can_values_t *values)
{
    if (payload == NULL || length == 0U) return false;
    if (payload[0] == VESC_COMM_GET_VALUES_SETUP_SELECTIVE) {
        return vesc_can_parse_setup_values(payload, length, values);
    }
    if (payload[0] == VESC_COMM_GET_DECODED_ADC) {
        return vesc_can_parse_decoded_adc(payload, length, values);
    }
    return false;
}

static void handle_reassembled_payload(uint8_t source_id, uint16_t length,
                                       uint16_t expected_crc, bool bridge)
{
    uint8_t *buffer = bridge ? s_bridge_rx_buffer : s_rx_buffer;
    const size_t capacity = bridge ? sizeof(s_bridge_rx_buffer) : sizeof(s_rx_buffer);
    if ((!bridge && source_id != EBIKE_VESC_CONTROLLER_ID) || length == 0U ||
        length > capacity) {
        snapshot_note_malformed();
        return;
    }

    if (vesc_can_crc16(buffer, length) != expected_crc) {
        snapshot_note_crc_error();
        return;
    }

    if (bridge) {
        vesc_tool_bridge_deliver_reply(buffer, length);
        return;
    }

    vesc_can_values_t values;
    if (!snapshot_copy_values(&values)) return;
    if (!parse_command_response(s_rx_buffer, length, &values)) {
        snapshot_note_malformed();
        return;
    }
    snapshot_accept_values(&values);
}

static void handle_short_payload(const twai_message_t *message, bool bridge)
{
    if (message->data_length_code < 3U ||
        (!bridge && message->data[0] != EBIKE_VESC_CONTROLLER_ID)) {
        snapshot_note_malformed();
        return;
    }

    if (bridge) {
        vesc_tool_bridge_deliver_reply(message->data + 2U,
                                       message->data_length_code - 2U);
        return;
    }

    vesc_can_values_t values;
    if (!snapshot_copy_values(&values)) return;
    if (!parse_command_response(message->data + 2U,
                                message->data_length_code - 2U, &values)) {
        snapshot_note_malformed();
        return;
    }
    snapshot_accept_values(&values);
}

static void handle_targeted_fragment(uint8_t packet_id,
                                     const twai_message_t *message, bool bridge)
{
    const uint8_t length = message->data_length_code;
    uint8_t *buffer = bridge ? s_bridge_rx_buffer : s_rx_buffer;
    const size_t capacity = bridge ? sizeof(s_bridge_rx_buffer) : sizeof(s_rx_buffer);

    if (packet_id == VESC_CAN_PACKET_FILL_RX_BUFFER) {
        if (length < 2U) {
            snapshot_note_malformed();
            return;
        }
        const uint16_t offset = message->data[0];
        const uint16_t bytes = (uint16_t)length - 1U;
        if (offset + bytes > capacity) {
            snapshot_note_malformed();
            return;
        }
        memcpy(buffer + offset, message->data + 1U, bytes);
        return;
    }

    if (packet_id == VESC_CAN_PACKET_FILL_RX_BUFFER_LONG) {
        if (length < 3U) {
            snapshot_note_malformed();
            return;
        }
        const uint16_t offset = ((uint16_t)message->data[0] << 8) | message->data[1];
        const uint16_t bytes = (uint16_t)length - 2U;
        if (offset + bytes > capacity) {
            snapshot_note_malformed();
            return;
        }
        memcpy(buffer + offset, message->data + 2U, bytes);
        return;
    }

    if (packet_id == VESC_CAN_PACKET_PROCESS_RX_BUFFER) {
        if (length < 6U) {
            snapshot_note_malformed();
            return;
        }
        const uint8_t source_id = message->data[0];
        const uint16_t payload_length = ((uint16_t)message->data[2] << 8) | message->data[3];
        const uint16_t crc = ((uint16_t)message->data[4] << 8) | message->data[5];
        handle_reassembled_payload(source_id, payload_length, crc, bridge);
        return;
    }

    if (packet_id == VESC_CAN_PACKET_PROCESS_SHORT_BUFFER) {
        handle_short_payload(message, bridge);
    }
}

static void handle_received_message(const twai_message_t *message)
{
    if (!message->extd || message->rtr) return;

    const uint8_t node_id = (uint8_t)(message->identifier & 0xFFU);
    const uint8_t packet_id = (uint8_t)((message->identifier >> 8) & 0xFFU);

    if (node_id == EBIKE_DASHBOARD_CAN_ID) {
        handle_targeted_fragment(packet_id, message, false);
        return;
    }

    if (node_id == VESC_TOOL_BRIDGE_CAN_ID) {
        handle_targeted_fragment(packet_id, message, true);
        return;
    }

    if (node_id != EBIKE_VESC_CONTROLLER_ID) return;

    vesc_can_values_t values;
    if (!snapshot_copy_values(&values)) return;
    if (vesc_can_parse_status(packet_id, message->data,
                              message->data_length_code, &values)) {
        snapshot_accept_values(&values);
    }
}

static void handle_alerts(void)
{
    uint32_t alerts = 0U;
    if (twai_read_alerts(&alerts, 0) != ESP_OK) return;

    if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGW(TAG, "CAN bus-off; starting recovery");
        if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_snapshot.bus_off_count++;
            s_snapshot.linked = false;
            xSemaphoreGive(s_snapshot_mutex);
        }
        esp_err_t err = twai_initiate_recovery();
        if (err != ESP_OK) ESP_LOGE(TAG, "CAN recovery start failed: %s", esp_err_to_name(err));
    }

    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGI(TAG, "CAN bus recovered");
        esp_err_t err = twai_start();
        if (err != ESP_OK) ESP_LOGE(TAG, "CAN restart failed: %s", esp_err_to_name(err));
    }

    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
        ESP_LOGW(TAG, "CAN RX queue full");
    }
}

static void can_task(void *argument)
{
    (void)argument;
    uint32_t next_setup_poll_ms = now_ms();
    uint32_t next_adc_poll_ms = next_setup_poll_ms +
                                (EBIKE_VESC_ADC_POLL_PERIOD_MS / 2U);
    uint32_t next_diagnostic_ms = next_setup_poll_ms + 5000U;

    while (true) {
        twai_message_t message;
        if (twai_receive(&message, pdMS_TO_TICKS(20)) == ESP_OK) {
            handle_received_message(&message);
        }
        handle_alerts();

        const uint32_t current_ms = now_ms();
        if ((int32_t)(current_ms - next_setup_poll_ms) >= 0) {
            esp_err_t err = transmit_setup_poll();
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "VESC poll failed: %s", esp_err_to_name(err));
            }
            next_setup_poll_ms = current_ms + EBIKE_VESC_POLL_PERIOD_MS;
        }

        if ((int32_t)(current_ms - next_adc_poll_ms) >= 0) {
            esp_err_t err = transmit_adc_poll();
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "VESC ADC poll failed: %s", esp_err_to_name(err));
            }
            next_adc_poll_ms = current_ms + EBIKE_VESC_ADC_POLL_PERIOD_MS;
        }

        bool link_timed_out = false;
        if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (s_snapshot.linked &&
                (current_ms - s_snapshot.last_valid_frame_ms) > EBIKE_VESC_LINK_TIMEOUT_MS) {
                s_snapshot.linked = false;
                link_timed_out = true;
            }
            xSemaphoreGive(s_snapshot_mutex);
        }
        if (link_timed_out) {
            ESP_LOGW(TAG, "VESC telemetry timeout; CAN link is down");
        }

        if ((int32_t)(current_ms - next_diagnostic_ms) >= 0) {
            ebike_can_snapshot_t diagnostic;
            if (ebike_can_get_snapshot(&diagnostic)) {
                ESP_LOGI(TAG,
                         "CAN diag: link=%s tx=%lu tx_err=%lu rx=%lu crc_err=%lu malformed=%lu bus_off=%lu",
                         diagnostic.linked ? "up" : "down",
                         (unsigned long)diagnostic.tx_poll_count,
                         (unsigned long)diagnostic.tx_error_count,
                         (unsigned long)diagnostic.rx_frame_count,
                         (unsigned long)diagnostic.crc_error_count,
                         (unsigned long)diagnostic.malformed_frame_count,
                         (unsigned long)diagnostic.bus_off_count);
            }
            next_diagnostic_ms = current_ms + 5000U;
        }
    }
}

bool ebike_can_get_snapshot(ebike_can_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_snapshot_mutex == NULL) return false;
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    *snapshot = s_snapshot;
    const uint32_t age_ms = now_ms() - snapshot->last_valid_frame_ms;
    snapshot->linked = snapshot->linked && age_ms <= EBIKE_VESC_LINK_TIMEOUT_MS;
    xSemaphoreGive(s_snapshot_mutex);
    return true;
}

esp_err_t ebike_can_start(void)
{
    if (s_driver_started) return ESP_ERR_INVALID_STATE;
    if (EBIKE_DASHBOARD_CAN_ID == EBIKE_VESC_CONTROLLER_ID) {
        ESP_LOGE(TAG, "Dashboard and VESC CAN IDs must be different");
        return ESP_ERR_INVALID_ARG;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) return ESP_ERR_NO_MEM;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_rx_buffer, 0, sizeof(s_rx_buffer));
    memset(s_bridge_rx_buffer, 0, sizeof(s_bridge_rx_buffer));

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        EBIKE_CAN_TX_GPIO, EBIKE_CAN_RX_GPIO, TWAI_MODE_NORMAL);
    general.tx_queue_len = 10;
    general.rx_queue_len = 32;
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                             TWAI_ALERT_RX_QUEUE_FULL;

#if EBIKE_CAN_BITRATE == 125000
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_125KBITS();
#elif EBIKE_CAN_BITRATE == 250000
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS();
#elif EBIKE_CAN_BITRATE == 500000
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
#elif EBIKE_CAN_BITRATE == 1000000
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_1MBITS();
#else
#error "EBIKE_CAN_BITRATE must be 125000, 250000, 500000, or 1000000"
#endif

    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t err = twai_driver_install(&general, &timing, &filter);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return err;
    }

    err = twai_start();
    if (err != ESP_OK) {
        twai_driver_uninstall();
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return err;
    }
    s_driver_started = true;

    if (xTaskCreatePinnedToCore(can_task, "vesc_can", 6144, NULL, 7,
                                &s_can_task, 0) != pdPASS) {
        s_driver_started = false;
        twai_stop();
        twai_driver_uninstall();
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TWAI started: TX GPIO%d, RX GPIO%d, %d bit/s, VESC ID %d",
             EBIKE_CAN_TX_GPIO, EBIKE_CAN_RX_GPIO, EBIKE_CAN_BITRATE,
             EBIKE_VESC_CONTROLLER_ID);
    return ESP_OK;
}
