#include "ebike_wifi.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "ebike_log.h"
#include "ebike_ota.h"
#include "ebike_spi_lock.h"

#define DOWNLOAD_BUFFER_SIZE 4096U

static const char *TAG = "ebike_wifi";
static httpd_handle_t s_server;
static char s_station_ssid[33];
static char s_station_password[65];
static bool s_station_configured;
static bool s_station_connected;

static void copy_string(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U) return;
    snprintf(destination, destination_size, "%s", source != NULL ? source : "");
}

static void html_escape(const char *source, char *destination, size_t destination_size)
{
    if (destination_size == 0U) return;
    size_t used = 0U;
    for (const char *p = source != NULL ? source : ""; *p != '\0' && used + 1U < destination_size;
         ++p) {
        const char *replacement = NULL;
        if (*p == '&') replacement = "&amp;";
        else if (*p == '<') replacement = "&lt;";
        else if (*p == '>') replacement = "&gt;";
        else if (*p == '\"') replacement = "&quot;";
        else if (*p == '\'') replacement = "&#39;";
        if (replacement == NULL) {
            destination[used++] = *p;
        } else {
            size_t length = strlen(replacement);
            if (used + length >= destination_size) break;
            memcpy(destination + used, replacement, length);
            used += length;
        }
    }
    destination[used] = '\0';
}

static const char *ota_state_name(ebike_ota_state_t state)
{
    switch (state) {
        case EBIKE_OTA_CHECKING: return "checking";
        case EBIKE_OTA_UP_TO_DATE: return "up to date";
        case EBIKE_OTA_AVAILABLE: return "update available";
        case EBIKE_OTA_DOWNLOADING: return "downloading";
        case EBIKE_OTA_RESTARTING: return "restarting";
        case EBIKE_OTA_ERROR: return "error";
        default: return "idle";
    }
}

static bool is_csv_log_name(const char *name)
{
    if (name == NULL || strncasecmp(name, "log", 3) != 0) return false;
    const size_t length = strlen(name);
    if (length < 8U || length > 16U ||
        strcasecmp(name + length - 4U, ".csv") != 0) return false;
    for (size_t i = 3U; i < length - 4U; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static esp_err_t send_page_header(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr_chunk(request,
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>E-bike logs</title><style>"
        "body{font:16px system-ui;background:#071317;color:#eef8fa;max-width:720px;margin:32px auto;padding:0 18px}"
        "h1{font-size:24px}p{color:#9eb6bc}.log{display:flex;justify-content:space-between;align-items:center;"
        "padding:13px 14px;margin:8px 0;background:#10272d;border-radius:10px}.active{color:#39d98a}"
        "a{color:#16ddf5;text-decoration:none;font-weight:600}.empty,.card{padding:16px;background:#10272d;border-radius:10px;margin:12px 0}"
        "label{display:block;color:#9eb6bc;margin:10px 0 5px}input{box-sizing:border-box;width:100%;padding:11px;border:1px solid #31515a;"
        "border-radius:8px;background:#071317;color:#eef8fa}button{padding:11px 15px;margin:12px 8px 0 0;border:0;border-radius:8px;"
        "background:#16ddf5;color:#001014;font-weight:700}.secondary{background:#27434a;color:#eef8fa}.ok{color:#39d98a}.warn{color:#ffb83d}"
        "</style></head><body><h1>E-bike CSV logs</h1>"
        "<p>Download a log, then open it in Excel, Google Sheets or another graphing tool.</p>");
}

static esp_err_t send_control_panel(httpd_req_t *request)
{
    ebike_ota_status_t ota;
    ebike_ota_get_status(&ota);
    char escaped_ssid[160];
    char escaped_message[256];
    html_escape(s_station_ssid, escaped_ssid, sizeof(escaped_ssid));
    html_escape(ota.message, escaped_message, sizeof(escaped_message));

    char panel[1400];
    snprintf(panel, sizeof(panel),
             "<h1>Internet and firmware</h1><div class=card>"
             "<b>Internet Wi-Fi: <span class=%s>%s</span></b>"
             "<form method=post action=/wifi>"
             "<label>Network name (SSID)</label><input name=ssid maxlength=31 value=\"%s\" required>"
             "<label>Password</label><input name=password type=password maxlength=63 placeholder='leave blank to keep saved password'>"
             "<button type=submit>SAVE AND CONNECT</button></form></div>"
             "<div class=card><b>Firmware %s</b><p>Status: %s<br>%s%s%s</p>"
             "<form method=post action=/ota/check style='display:inline'><button type=submit>CHECK NOW</button></form>"
             "%s</div>",
             s_station_connected ? "ok" : "warn",
             s_station_connected ? "connected" : (s_station_configured ? "disconnected" : "not configured"),
             escaped_ssid, ota.installed_version, ota_state_name(ota.state), escaped_message,
             ota.available_version[0] != '\0' ? "<br>Available: " : "",
             ota.available_version,
             ota.state == EBIKE_OTA_AVAILABLE
                 ? "<form method=post action=/ota/install style='display:inline'><button type=submit>INSTALL UPDATE</button></form>"
                 : "");
    return httpd_resp_sendstr_chunk(request, panel);
}

static esp_err_t index_handler(httpd_req_t *request)
{
    esp_err_t result = send_page_header(request);
    if (result != ESP_OK) return result;

    (void)ebike_log_sync();
    const char *active_path = ebike_log_filename();
    const char *active_name = active_path != NULL ? strrchr(active_path, '/') : NULL;
    if (active_name != NULL) active_name++;

    DIR *directory = NULL;
    if (!ebike_spi_lock_take(pdMS_TO_TICKS(1000))) {
        result = httpd_resp_sendstr_chunk(request,
                                          "<div class=empty>SD card is busy. Refresh the page.</div>");
    } else {
        directory = opendir(EBIKE_LOG_MOUNT_POINT);
        ebike_spi_lock_give();
        if (directory == NULL) {
            result = httpd_resp_sendstr_chunk(request,
                                              "<div class=empty>No mounted SD card.</div>");
        }
    }

    unsigned count = 0U;
    while (result == ESP_OK && directory != NULL) {
        if (!ebike_spi_lock_take(pdMS_TO_TICKS(1000))) break;
        errno = 0;
        struct dirent *entry = readdir(directory);
        char name[20] = {0};
        const bool end_of_directory = entry == NULL;
        bool name_fits = false;
        if (entry != NULL) {
            const size_t name_length = strnlen(entry->d_name, sizeof(name));
            name_fits = name_length < sizeof(name);
            if (name_fits) memcpy(name, entry->d_name, name_length + 1U);
        }
        ebike_spi_lock_give();
        if (end_of_directory) break;
        if (!name_fits) continue;
        if (!is_csv_log_name(name)) continue;

        char row[192];
        const bool active = active_name != NULL && strcasecmp(name, active_name) == 0;
        snprintf(row, sizeof(row),
                 "<div class=log><span%s>%s%s</span><a href='/download?file=%s'>Download</a></div>",
                 active ? " class=active" : "", name, active ? " (recording)" : "", name);
        result = httpd_resp_sendstr_chunk(request, row);
        if (result != ESP_OK) break;
        count++;
    }

    if (directory != NULL && ebike_spi_lock_take(portMAX_DELAY)) {
        closedir(directory);
        ebike_spi_lock_give();
    }
    if (result == ESP_OK && directory != NULL && count == 0U) {
        result = httpd_resp_sendstr_chunk(request, "<div class=empty>No CSV logs found.</div>");
    }
    ESP_LOGI(TAG, "HTTP log list: %u CSV file(s)", count);
    if (result == ESP_OK) result = send_control_panel(request);
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(request, "</body></html>");
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static void url_decode(char *text)
{
    char *read = text;
    char *write = text;
    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (*read == '%' && read[1] != '\0' && read[2] != '\0') {
            int high = hex_value(read[1]);
            int low = hex_value(read[2]);
            if (high >= 0 && low >= 0) {
                *write++ = (char)((high << 4) | low);
                read += 3;
            } else {
                *write++ = *read++;
            }
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static bool form_value(const char *body, const char *key, char *value, size_t value_size)
{
    if (body == NULL || key == NULL || value == NULL || value_size == 0U) return false;
    const size_t key_length = strlen(key);
    const char *position = body;
    while (position != NULL && *position != '\0') {
        if ((position == body || position[-1] == '&') &&
            strncmp(position, key, key_length) == 0 && position[key_length] == '=') {
            position += key_length + 1U;
            const char *end = strchr(position, '&');
            size_t length = end != NULL ? (size_t)(end - position) : strlen(position);
            if (length >= value_size) return false;
            memcpy(value, position, length);
            value[length] = '\0';
            url_decode(value);
            return true;
        }
        position = strchr(position, '&');
        if (position != NULL) position++;
    }
    return false;
}

static esp_err_t read_request_body(httpd_req_t *request, char *body, size_t body_size)
{
    if (request->content_len <= 0 || (size_t)request->content_len >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received_total = 0U;
    while (received_total < (size_t)request->content_len) {
        int received = httpd_req_recv(request, body + received_total,
                                      request->content_len - received_total);
        if (received <= 0) return ESP_FAIL;
        received_total += (size_t)received;
    }
    body[received_total] = '\0';
    return ESP_OK;
}

static esp_err_t redirect_home(httpd_req_t *request)
{
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t save_station_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ebike_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void configure_and_connect_station(void)
{
    wifi_config_t station = {0};
    copy_string((char *)station.sta.ssid, sizeof(station.sta.ssid), s_station_ssid);
    copy_string((char *)station.sta.password, sizeof(station.sta.password), s_station_password);
    station.sta.threshold.authmode = strlen(s_station_password) > 0U
                                         ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

static esp_err_t wifi_form_handler(httpd_req_t *request)
{
    char body[256];
    char ssid[33];
    char password[65];
    esp_err_t err = read_request_body(request, body, sizeof(body));
    if (err != ESP_OK || !form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid SSID");
    }
    bool password_present = form_value(body, "password", password, sizeof(password));
    if (!password_present || password[0] == '\0') {
        copy_string(password, sizeof(password), s_station_password);
    }
    const size_t password_length = strlen(password);
    if (password_length > 0U && password_length < 8U) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Wi-Fi password must be empty or at least 8 characters");
    }
    err = save_station_credentials(ssid, password);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save Wi-Fi settings");
    }
    copy_string(s_station_ssid, sizeof(s_station_ssid), ssid);
    copy_string(s_station_password, sizeof(s_station_password), password);
    s_station_configured = true;
    s_station_connected = false;
    ebike_ota_notify_network(false);
    (void)esp_wifi_disconnect();
    configure_and_connect_station();
    return redirect_home(request);
}

static esp_err_t ota_check_handler(httpd_req_t *request)
{
    ebike_ota_request_check();
    return redirect_home(request);
}

static esp_err_t ota_install_handler(httpd_req_t *request)
{
    esp_err_t err = ebike_ota_install();
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_send(request, esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
    }
    return redirect_home(request);
}

static void load_station_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open("ebike_wifi", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t ssid_size = sizeof(s_station_ssid);
    size_t password_size = sizeof(s_station_password);
    esp_err_t ssid_err = nvs_get_str(nvs, "ssid", s_station_ssid, &ssid_size);
    esp_err_t password_err = nvs_get_str(nvs, "password", s_station_password, &password_size);
    nvs_close(nvs);
    s_station_configured = ssid_err == ESP_OK && password_err == ESP_OK &&
                           s_station_ssid[0] != '\0';
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_station_connected = true;
        ebike_ota_notify_network(true);
        ESP_LOGI(TAG, "Internet Wi-Fi connected: %s", s_station_ssid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_station_connected = false;
        ebike_ota_notify_network(false);
        if (s_station_configured) (void)esp_wifi_connect();
    }
}

static esp_err_t download_handler(httpd_req_t *request)
{
    char query[64];
    char name[20];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", name, sizeof(name)) != ESP_OK ||
        !is_csv_log_name(name)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid log filename");
    }

    (void)ebike_log_sync();
    char path[48];
    snprintf(path, sizeof(path), EBIKE_LOG_MOUNT_POINT "/%s", name);

    if (!ebike_spi_lock_take(pdMS_TO_TICKS(1000))) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_send(request, "SD card is busy", HTTPD_RESP_USE_STRLEN);
    }
    FILE *file = fopen(path, "rb");
    long snapshot_size = -1;
    if (file != NULL && fseek(file, 0, SEEK_END) == 0) {
        snapshot_size = ftell(file);
        if (snapshot_size >= 0) rewind(file);
    }
    if (file == NULL || snapshot_size < 0) {
        if (file != NULL) fclose(file);
        ebike_spi_lock_give();
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Log not found");
    }
    ebike_spi_lock_give();

    char *buffer = malloc(DOWNLOAD_BUFFER_SIZE);
    if (buffer == NULL) {
        if (ebike_spi_lock_take(portMAX_DELAY)) {
            fclose(file);
            ebike_spi_lock_give();
        }
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Not enough memory for download");
    }

    httpd_resp_set_type(request, "text/csv");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char disposition[64];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(request, "Content-Disposition", disposition);

    long remaining = snapshot_size;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        const size_t requested = remaining < (long)DOWNLOAD_BUFFER_SIZE
                                     ? (size_t)remaining : DOWNLOAD_BUFFER_SIZE;
        if (!ebike_spi_lock_take(pdMS_TO_TICKS(1000))) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        const size_t read_count = fread(buffer, 1, requested, file);
        const bool read_failed = read_count == 0U && ferror(file);
        ebike_spi_lock_give();
        if (read_failed || read_count == 0U) {
            result = ESP_FAIL;
            break;
        }
        result = httpd_resp_send_chunk(request, buffer, read_count);
        if (result != ESP_OK) break;
        remaining -= (long)read_count;
    }

    if (ebike_spi_lock_take(portMAX_DELAY)) {
        fclose(file);
        ebike_spi_lock_give();
    }
    free(buffer);
    if (result == ESP_OK && remaining == 0) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
}

esp_err_t ebike_wifi_start(void)
{
    if (s_server != NULL) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (esp_netif_create_default_wifi_ap() == NULL) return ESP_FAIL;
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_FAIL;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "Wi-Fi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "IP event handler failed");
    load_station_credentials();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EBIKE_WIFI_SSID,
            .password = EBIKE_WIFI_PASSWORD,
            .ssid_len = sizeof(EBIKE_WIFI_SSID) - 1U,
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2,
            .pmf_cfg = {.required = false},
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "AP+STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "AP config failed");
    if (s_station_configured) {
        wifi_config_t station = {0};
        copy_string((char *)station.sta.ssid, sizeof(station.sta.ssid), s_station_ssid);
        copy_string((char *)station.sta.password, sizeof(station.sta.password),
                    s_station_password);
        station.sta.threshold.authmode = strlen(s_station_password) > 0U
                                             ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        station.sta.pmf_cfg.capable = true;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), TAG,
                            "STA config failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start failed");
    if (s_station_configured) (void)esp_wifi_connect();

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 6144;
    server_config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &server_config), TAG, "HTTP server failed");

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    const httpd_uri_t download_uri = {
        .uri = "/download",
        .method = HTTP_GET,
        .handler = download_handler,
    };
    const httpd_uri_t wifi_uri = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_form_handler,
    };
    const httpd_uri_t ota_check_uri = {
        .uri = "/ota/check",
        .method = HTTP_POST,
        .handler = ota_check_handler,
    };
    const httpd_uri_t ota_install_uri = {
        .uri = "/ota/install",
        .method = HTTP_POST,
        .handler = ota_install_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &index_uri), TAG, "Index route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &download_uri), TAG, "Download route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &wifi_uri), TAG, "Wi-Fi route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &ota_check_uri), TAG, "OTA check route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &ota_install_uri), TAG, "OTA install route failed");

    ESP_LOGI(TAG, "Connect to %s (password: %s), then open http://192.168.4.1/",
             EBIKE_WIFI_SSID, EBIKE_WIFI_PASSWORD);
    return ESP_OK;
}
