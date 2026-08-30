idf_component_register(
    SRCS
        "bsp_qmi8658.c"
        "main.c"
        "ebike_ui.c"
        "ebike_can.c"
        "ebike_log.c"
        "ebike_wifi.c"
        "ebike_ota.c"
        "vesc_ble.c"
        "vesc_tool_bridge.c"
        "ebike_spi_lock.c"
        "ebike_trip.c"
        "vesc_can_protocol.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_ledc esp_driver_twai esp_driver_sdspi fatfs sdmmc nvs_flash
             esp_wifi esp_http_server esp_http_client esp_netif esp_event bt app_update
             mbedtls json
)
