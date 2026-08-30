#include "vesc_ble.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "vesc_can_protocol.h"
#include "vesc_tool_bridge.h"

#define BLE_STREAM_BUFFER_SIZE (VESC_TOOL_MAX_PAYLOAD + 8U)
#define BLE_DEFAULT_CHUNK_SIZE 20U

static const char *TAG = "vesc_ble";
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_value_handle;
static uint16_t s_peer_mtu = 23U;
static uint8_t s_stream[BLE_STREAM_BUFFER_SIZE];
static size_t s_stream_length;
static SemaphoreHandle_t s_ble_mutex;

static const ble_uuid128_t s_nus_service_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static void parse_stream_locked(void)
{
    while (s_stream_length > 0U) {
        size_t header_length;
        size_t payload_length;
        if (s_stream[0] == 2U) {
            if (s_stream_length < 2U) return;
            header_length = 2U;
            payload_length = s_stream[1];
        } else if (s_stream[0] == 3U) {
            if (s_stream_length < 3U) return;
            header_length = 3U;
            payload_length = ((size_t)s_stream[1] << 8) | s_stream[2];
            if (payload_length < 255U) {
                memmove(s_stream, s_stream + 1U, --s_stream_length);
                continue;
            }
        } else {
            memmove(s_stream, s_stream + 1U, --s_stream_length);
            continue;
        }

        if (payload_length == 0U || payload_length > VESC_TOOL_MAX_PAYLOAD) {
            memmove(s_stream, s_stream + 1U, --s_stream_length);
            continue;
        }
        const size_t frame_length = header_length + payload_length + 3U;
        if (s_stream_length < frame_length) return;

        const uint8_t *payload = s_stream + header_length;
        const uint16_t received_crc =
            ((uint16_t)payload[payload_length] << 8) | payload[payload_length + 1U];
        if (payload[payload_length + 2U] == 3U &&
            vesc_can_crc16(payload, payload_length) == received_crc) {
            esp_err_t err = vesc_tool_bridge_send(payload, payload_length);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "CAN bridge send failed: %s", esp_err_to_name(err));
            }
            memmove(s_stream, s_stream + frame_length,
                    s_stream_length - frame_length);
            s_stream_length -= frame_length;
        } else {
            memmove(s_stream, s_stream + 1U, --s_stream_length);
        }
    }
}

static int gatt_access(uint16_t connection_handle, uint16_t attribute_handle,
                       struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t incoming = OS_MBUF_PKTLEN(context->om);
    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if ((size_t)incoming > sizeof(s_stream) - s_stream_length) {
        s_stream_length = 0U;
        xSemaphoreGive(s_ble_mutex);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    const int result = ble_hs_mbuf_to_flat(context->om,
                                            s_stream + s_stream_length,
                                            incoming, NULL);
    if (result == 0) {
        s_stream_length += incoming;
        parse_stream_locked();
    }
    xSemaphoreGive(s_ble_mutex);
    return result == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_nus_rx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_nus_tx_uuid.u,
                .access_cb = gatt_access,
                .val_handle = &s_tx_value_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_peer_mtu = ble_att_mtu(s_conn_handle);
            ESP_LOGI(TAG, "VESC Tool BLE connected");
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_peer_mtu = 23U;
        ESP_LOGI(TAG, "VESC Tool BLE disconnected");
        advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_peer_mtu = event->mtu.value;
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

bool vesc_ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* Keep the complete NUS UUID in the 31-byte advertisement. VESC Tool on
     * Android filters scan results by this UUID before it offers a device. */
    static const char advertised_name[] = "VESC-E";
    const char *name = advertised_name;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&s_nus_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "BLE advertising fields failed: %d", result);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &parameters, gap_event, NULL);
    if (result != 0) ESP_LOGE(TAG, "BLE advertising failed: %d", result);
}

static void host_sync(void)
{
    int result = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (result != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", result);
        return;
    }
    advertise();
}

static void host_reset(int reason)
{
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_peer_mtu = 23U;
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void send_reply(const uint8_t *payload, size_t length, void *user_data)
{
    (void)user_data;
    if (payload == NULL || length == 0U || length > VESC_TOOL_MAX_PAYLOAD ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    uint8_t frame[VESC_TOOL_MAX_PAYLOAD + 6U];
    size_t index = 0U;
    if (length <= 255U) {
        frame[index++] = 2U;
        frame[index++] = (uint8_t)length;
    } else {
        frame[index++] = 3U;
        frame[index++] = (uint8_t)(length >> 8);
        frame[index++] = (uint8_t)length;
    }
    memcpy(frame + index, payload, length);
    index += length;
    const uint16_t crc = vesc_can_crc16(payload, length);
    frame[index++] = (uint8_t)(crc >> 8);
    frame[index++] = (uint8_t)crc;
    frame[index++] = 3U;

    size_t offset = 0U;
    const size_t chunk_size = s_peer_mtu > 3U ? (size_t)s_peer_mtu - 3U
                                               : BLE_DEFAULT_CHUNK_SIZE;
    while (offset < index && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        const size_t bytes = (index - offset) < chunk_size ? (index - offset)
                                                           : chunk_size;
        struct os_mbuf *packet = ble_hs_mbuf_from_flat(frame + offset, bytes);
        if (packet == NULL || ble_gatts_notify_custom(s_conn_handle,
                                                       s_tx_value_handle,
                                                       packet) != 0) {
            ESP_LOGW(TAG, "BLE reply notification failed");
            break;
        }
        offset += bytes;
    }
}

esp_err_t vesc_ble_start(void)
{
    s_ble_mutex = xSemaphoreCreateMutex();
    if (s_ble_mutex == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int result = ble_svc_gap_device_name_set("VESC-E");
    if (result != 0) {
        ESP_LOGE(TAG, "BLE device name failed: rc=%d", result);
        return ESP_FAIL;
    }
    result = ble_gatts_count_cfg(s_gatt_services);
    if (result != 0) {
        ESP_LOGE(TAG, "BLE GATT count failed: rc=%d", result);
        return ESP_FAIL;
    }
    result = ble_gatts_add_svcs(s_gatt_services);
    if (result != 0) {
        ESP_LOGE(TAG, "BLE GATT service registration failed: rc=%d", result);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.reset_cb = host_reset;
    vesc_tool_bridge_set_reply_callback(send_reply, NULL);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "VESC Tool BLE bridge started as VESC-E");
    return ESP_OK;
}
