#include "ebike_ota.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "ebike_can.h"
#include "vesc_can_protocol.h"

#define OTA_MANIFEST_MAX_BYTES 2048U
#define OTA_DOWNLOAD_BUFFER_BYTES 4096U
#define OTA_CHECK_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define OTA_FIRST_CHECK_DELAY_MS 10000U

typedef struct {
    char version[32];
    char firmware_url[384];
    char sha256[65];
    char release_notes[160];
    uint32_t size;
} ota_manifest_t;

static const char *TAG = "ebike_ota";
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_check_task;
static TaskHandle_t s_install_task;
static ebike_ota_status_t s_status;
static ota_manifest_t s_manifest;
static ebike_ota_event_cb_t s_callback;
static void *s_callback_user_data;
static bool s_first_network_check = true;

static void copy_string(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U) return;
    if (source == NULL) source = "";
    snprintf(destination, destination_size, "%s", source);
}

static void publish_status(void)
{
    ebike_ota_status_t snapshot;
    ebike_ota_event_cb_t callback;
    void *user_data;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    snapshot = s_status;
    callback = s_callback;
    user_data = s_callback_user_data;
    xSemaphoreGive(s_mutex);
    if (callback != NULL) callback(&snapshot, user_data);
}

static void set_state(ebike_ota_state_t state, const char *message)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    s_status.state = state;
    copy_string(s_status.message, sizeof(s_status.message), message);
    xSemaphoreGive(s_mutex);
    publish_status();
}

static bool parse_version(const char *text, unsigned parts[3])
{
    if (text == NULL || parts == NULL) return false;
    if (*text == 'v' || *text == 'V') text++;
    for (unsigned i = 0; i < 3U; ++i) {
        if (!isdigit((unsigned char)*text)) return false;
        char *end = NULL;
        unsigned long value = strtoul(text, &end, 10);
        if (end == text || value > 9999UL) return false;
        parts[i] = (unsigned)value;
        text = end;
        if (i < 2U) {
            if (*text != '.') return false;
            text++;
        }
    }
    return *text == '\0' || *text == '-' || *text == '+';
}

static int compare_versions(const char *left, const char *right)
{
    unsigned a[3];
    unsigned b[3];
    if (!parse_version(left, a) || !parse_version(right, b)) return 0;
    for (unsigned i = 0; i < 3U; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    const bool left_prerelease = strchr(left, '-') != NULL;
    const bool right_prerelease = strchr(right, '-') != NULL;
    if (left_prerelease != right_prerelease) return left_prerelease ? -1 : 1;
    return 0;
}

static bool json_string(cJSON *root, const char *name, char *destination,
                        size_t destination_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return false;
    }
    copy_string(destination, destination_size, item->valuestring);
    return strlen(item->valuestring) < destination_size;
}

static esp_err_t parse_manifest(const char *json, ota_manifest_t *manifest)
{
    if (json == NULL || manifest == NULL) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;

    char product[48];
    char hardware[64];
    char channel[24];
    ota_manifest_t parsed = {0};
    bool valid = json_string(root, "product", product, sizeof(product)) &&
                 json_string(root, "hardware", hardware, sizeof(hardware)) &&
                 json_string(root, "channel", channel, sizeof(channel)) &&
                 json_string(root, "version", parsed.version, sizeof(parsed.version)) &&
                 json_string(root, "firmware_url", parsed.firmware_url,
                             sizeof(parsed.firmware_url)) &&
                 json_string(root, "sha256", parsed.sha256, sizeof(parsed.sha256));
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0.0 ||
        size->valuedouble > (double)UINT32_MAX) {
        valid = false;
    } else {
        parsed.size = (uint32_t)size->valuedouble;
    }
    cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "release_notes");
    if (cJSON_IsString(notes) && notes->valuestring != NULL) {
        copy_string(parsed.release_notes, sizeof(parsed.release_notes), notes->valuestring);
    }
    cJSON_Delete(root);

    if (!valid || strcmp(product, EBIKE_OTA_PRODUCT) != 0 ||
        strcmp(hardware, EBIKE_OTA_HARDWARE) != 0 ||
        strcmp(channel, EBIKE_OTA_CHANNEL) != 0 || strlen(parsed.sha256) != 64U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    unsigned version_parts[3];
    if (!parse_version(parsed.version, version_parts)) return ESP_ERR_INVALID_VERSION;
    for (size_t i = 0; i < 64U; ++i) {
        if (!isxdigit((unsigned char)parsed.sha256[i])) return ESP_ERR_INVALID_CRC;
        parsed.sha256[i] = (char)tolower((unsigned char)parsed.sha256[i]);
    }
    *manifest = parsed;
    return ESP_OK;
}

typedef struct {
    char *buffer;
    size_t used;
    esp_err_t error;
} manifest_http_context_t;

static esp_err_t manifest_http_event(esp_http_client_event_t *event)
{
    manifest_http_context_t *context = event != NULL ? event->user_data : NULL;
    if (context == NULL || context->error != ESP_OK) return context != NULL ? context->error
                                                                            : ESP_ERR_INVALID_ARG;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if (context->used + (size_t)event->data_len > OTA_MANIFEST_MAX_BYTES) {
        context->error = ESP_ERR_INVALID_SIZE;
        return context->error;
    }
    memcpy(context->buffer + context->used, event->data, (size_t)event->data_len);
    context->used += (size_t)event->data_len;
    return ESP_OK;
}

static esp_err_t fetch_manifest(ota_manifest_t *manifest)
{
    char *buffer = calloc(1U, OTA_MANIFEST_MAX_BYTES + 1U);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    manifest_http_context_t context = {.buffer = buffer, .error = ESP_OK};
    const esp_http_client_config_t config = {
        .url = EBIKE_OTA_MANIFEST_URL,
        .event_handler = manifest_http_event,
        .user_data = &context,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .user_agent = "ebike-dashboard-ota/1",
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) err = context.error;
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200) {
        err = ESP_ERR_HTTP_FETCH_HEADER;
    }
    buffer[context.used] = '\0';
    if (err == ESP_OK) err = parse_manifest(buffer, manifest);

    esp_http_client_cleanup(client);
    free(buffer);
    return err;
}

static void perform_check(void)
{
    bool connected = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        connected = s_status.network_connected;
        xSemaphoreGive(s_mutex);
    }
    if (!connected) return;

    set_state(EBIKE_OTA_CHECKING, "Checking for updates");
    ota_manifest_t manifest;
    esp_err_t err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        char message[128];
        snprintf(message, sizeof(message), "Update check failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", message);
        set_state(EBIKE_OTA_ERROR, message);
        return;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL || manifest.size > update_partition->size) {
        set_state(EBIKE_OTA_ERROR, "Firmware does not fit the OTA slot");
        return;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (compare_versions(running->version, manifest.version) >= 0) {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_status.state = EBIKE_OTA_UP_TO_DATE;
            s_status.available_version[0] = '\0';
            s_status.image_size = 0U;
            copy_string(s_status.message, sizeof(s_status.message), "Firmware is up to date");
            xSemaphoreGive(s_mutex);
        }
        publish_status();
        return;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_manifest = manifest;
        s_status.state = EBIKE_OTA_AVAILABLE;
        copy_string(s_status.available_version, sizeof(s_status.available_version), manifest.version);
        copy_string(s_status.release_notes, sizeof(s_status.release_notes), manifest.release_notes);
        copy_string(s_status.message, sizeof(s_status.message), "Update available");
        s_status.image_size = manifest.size;
        s_status.progress_percent = 0U;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Firmware %s is available (%lu bytes)", manifest.version,
             (unsigned long)manifest.size);
    publish_status();
}

static void check_task(void *parameter)
{
    (void)parameter;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
        bool should_delay = false;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            should_delay = s_first_network_check && s_status.network_connected;
            if (should_delay) s_first_network_check = false;
            xSemaphoreGive(s_mutex);
        }
        if (should_delay) vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_DELAY_MS));
        perform_check();
    }
}

static bool decode_sha256(const char *text, uint8_t output[32])
{
    if (text == NULL || strlen(text) != 64U) return false;
    for (size_t i = 0; i < 32U; ++i) {
        char pair[3] = {text[i * 2U], text[i * 2U + 1U], '\0'};
        char *end = NULL;
        unsigned long value = strtoul(pair, &end, 16);
        if (end == NULL || *end != '\0') return false;
        output[i] = (uint8_t)value;
    }
    return true;
}

static bool update_is_safe(void)
{
    ebike_can_snapshot_t snapshot;
    if (!ebike_can_get_snapshot(&snapshot) || !snapshot.linked) return true;
    const vesc_can_values_t *values = &snapshot.values;
    if ((values->valid_mask & VESC_VALUE_SPEED) && fabsf(values->speed_mps) > 0.3f) return false;
    if ((values->valid_mask & VESC_VALUE_RPM) && fabsf(values->rpm) > 100.0f) return false;
    if ((values->valid_mask & VESC_VALUE_INPUT_CURRENT) &&
        fabsf(values->input_current_a) > 2.0f) return false;
    return true;
}

static esp_err_t validate_first_chunk(const uint8_t *data, size_t length,
                                      const ota_manifest_t *manifest)
{
    const size_t descriptor_offset = sizeof(esp_image_header_t) +
                                     sizeof(esp_image_segment_header_t);
    if (length < descriptor_offset + sizeof(esp_app_desc_t)) return ESP_ERR_INVALID_SIZE;
    if (data[0] != ESP_IMAGE_HEADER_MAGIC) return ESP_ERR_OTA_VALIDATE_FAILED;
    const esp_app_desc_t *description = (const esp_app_desc_t *)(data + descriptor_offset);
    if (strncmp(description->project_name, "ebike_dashboard",
                sizeof(description->project_name)) != 0) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (strncmp(description->version, manifest->version, sizeof(description->version)) != 0) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

typedef struct {
    const ota_manifest_t *manifest;
    esp_ota_handle_t ota_handle;
    mbedtls_sha256_context *sha_context;
    uint8_t header[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                   sizeof(esp_app_desc_t)];
    size_t header_used;
    uint32_t total;
    uint8_t last_progress;
    bool header_validated;
    esp_err_t error;
} firmware_http_context_t;

static esp_err_t write_firmware_bytes(firmware_http_context_t *context,
                                      const uint8_t *data, size_t length)
{
    if (context->total + length > context->manifest->size) return ESP_ERR_INVALID_SIZE;
    if (mbedtls_sha256_update(context->sha_context, data, length) != 0) return ESP_FAIL;
    esp_err_t err = esp_ota_write(context->ota_handle, data, length);
    if (err != ESP_OK) return err;
    context->total += (uint32_t)length;
    uint8_t progress = (uint8_t)(((uint64_t)context->total * 100U) /
                                 context->manifest->size);
    if (progress != context->last_progress) {
        context->last_progress = progress;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_status.progress_percent = progress;
            xSemaphoreGive(s_mutex);
        }
        publish_status();
    }
    return ESP_OK;
}

static esp_err_t firmware_http_event(esp_http_client_event_t *event)
{
    firmware_http_context_t *context = event != NULL ? event->user_data : NULL;
    if (context == NULL || context->error != ESP_OK) return context != NULL ? context->error
                                                                            : ESP_ERR_INVALID_ARG;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;

    const uint8_t *data = event->data;
    size_t length = (size_t)event->data_len;
    if (!context->header_validated) {
        const size_t required = sizeof(context->header) - context->header_used;
        const size_t copied = length < required ? length : required;
        memcpy(context->header + context->header_used, data, copied);
        context->header_used += copied;
        data += copied;
        length -= copied;
        if (context->header_used == sizeof(context->header)) {
            context->error = validate_first_chunk(context->header, sizeof(context->header),
                                                  context->manifest);
            if (context->error != ESP_OK) return context->error;
            context->header_validated = true;
            context->error = write_firmware_bytes(context, context->header,
                                                  sizeof(context->header));
            if (context->error != ESP_OK) return context->error;
        }
    }
    if (context->header_validated && length > 0U) {
        context->error = write_firmware_bytes(context, data, length);
    }
    return context->error;
}

static esp_err_t download_and_stage(const ota_manifest_t *manifest)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || manifest->size > partition->size) return ESP_ERR_INVALID_SIZE;

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, manifest->size, &ota_handle);
    if (err != ESP_OK) return err;

    mbedtls_sha256_context sha_context;
    mbedtls_sha256_init(&sha_context);
    if (err == ESP_OK && mbedtls_sha256_starts(&sha_context, 0) != 0) err = ESP_FAIL;
    firmware_http_context_t context = {
        .manifest = manifest,
        .ota_handle = ota_handle,
        .sha_context = &sha_context,
        .error = err,
    };
    const esp_http_client_config_t config = {
        .url = manifest->firmware_url,
        .event_handler = firmware_http_event,
        .user_data = &context,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = OTA_DOWNLOAD_BUFFER_BYTES,
        .user_agent = "ebike-dashboard-ota/1",
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = NULL;
    if (err == ESP_OK) {
        client = esp_http_client_init(&config);
        if (client == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = esp_http_client_perform(client);
    if (err == ESP_OK) err = context.error;
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200) {
        err = ESP_ERR_HTTP_FETCH_HEADER;
    }

    uint8_t calculated_hash[32];
    uint8_t expected_hash[32];
    if (err == ESP_OK && (!context.header_validated || context.total != manifest->size)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && mbedtls_sha256_finish(&sha_context, calculated_hash) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && (!decode_sha256(manifest->sha256, expected_hash) ||
                          memcmp(calculated_hash, expected_hash, sizeof(calculated_hash)) != 0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    mbedtls_sha256_free(&sha_context);

    if (client != NULL) esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        (void)esp_ota_abort(ota_handle);
        return err;
    }
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) return err;
    return esp_ota_set_boot_partition(partition);
}

static void install_task(void *parameter)
{
    (void)parameter;
    ota_manifest_t manifest;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        manifest = s_manifest;
        s_status.state = EBIKE_OTA_DOWNLOADING;
        s_status.progress_percent = 0U;
        copy_string(s_status.message, sizeof(s_status.message), "Downloading update");
        xSemaphoreGive(s_mutex);
    }
    publish_status();

    esp_err_t err = download_and_stage(&manifest);
    if (err != ESP_OK) {
        char message[128];
        snprintf(message, sizeof(message), "Update failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", message);
        set_state(EBIKE_OTA_ERROR, message);
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_install_task = NULL;
            xSemaphoreGive(s_mutex);
        }
        vTaskDelete(NULL);
        return;
    }

    set_state(EBIKE_OTA_RESTARTING, "Update verified; restarting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t ebike_ota_init(ebike_ota_event_cb_t callback, void *user_data)
{
    if (s_mutex != NULL) return ESP_ERR_INVALID_STATE;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    const esp_app_desc_t *description = esp_app_get_description();
    copy_string(s_status.installed_version, sizeof(s_status.installed_version),
                description->version);
    copy_string(s_status.message, sizeof(s_status.message), "OTA ready");
    s_callback = callback;
    s_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t ebike_ota_start(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (s_check_task != NULL) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(check_task, "ota_check", 6144, NULL, 3, &s_check_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    bool connected = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        connected = s_status.network_connected;
        xSemaphoreGive(s_mutex);
    }
    if (connected) xTaskNotifyGive(s_check_task);
    return ESP_OK;
}

void ebike_ota_notify_network(bool connected)
{
    if (s_mutex == NULL) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.network_connected = connected;
        xSemaphoreGive(s_mutex);
    }
    if (connected && s_check_task != NULL) xTaskNotifyGive(s_check_task);
    publish_status();
}

void ebike_ota_request_check(void)
{
    if (s_check_task != NULL) xTaskNotifyGive(s_check_task);
}

esp_err_t ebike_ota_install(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (!update_is_safe()) {
        set_state(EBIKE_OTA_ERROR, "Stop the bicycle before installing");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_status.network_connected || s_status.state != EBIKE_OTA_AVAILABLE ||
        s_install_task != NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t result = xTaskCreate(install_task, "ota_install", 8192, NULL, 4,
                                    &s_install_task);
    xSemaphoreGive(s_mutex);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void ebike_ota_get_status(ebike_ota_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    *status = s_status;
    xSemaphoreGive(s_mutex);
}

esp_err_t ebike_ota_confirm_running_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) return err;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) ESP_LOGI(TAG, "New OTA image passed startup self-test");
    return err;
}
