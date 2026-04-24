set(APP_ROOT_SOURCES
    "main.c"
)

set(BSP_DRIVER_SOURCES
    "bsp_driver/ble.c"
    "bsp_driver/factory_cfg.c"
    "bsp_driver/led.c"
    "bsp_driver/mqtt.c"
    "bsp_driver/spiffs.c"
    "bsp_driver/uart_ctr.c"
    "bsp_driver/uart_hmi.c"
    "bsp_driver/wifi.c"
)

set(MQTT_AUTH_SOURCES
    "mqtt_auth/mqtt_auth.c"
    "mqtt_auth/RSA_sign.c"
)

set(OTA_SOURCES
    "ota_ctr_esp32/ota_bundle.c"
    "ota_ctr_esp32/ota_ctr.c"
    "ota_ctr_esp32/ota_esp32.c"
    "ota_ctr_esp32/Ymodem_esp.c"
)

set(SERVICE_SOURCES
    "service/circular_buff.c"
    "service/ctr_scheduler.c"
    "service/device_statistics_store.c"
    "service/drink_record_service.c"
    "service/event_record_service.c"
    "service/formula_store.c"
    "service/led_service.c"
    "service/ram_diag.c"
)

set(PROTOCOL_SOURCES
    "sp_protocol/mqtt_protocol.c"
    "sp_protocol/mqtt_protocol_control.c"
    "sp_protocol/mqtt_protocol_core.c"
    "sp_protocol/mqtt_protocol_formula.c"
    "sp_protocol/mqtt_protocol_ota.c"
    "sp_protocol/mqtt_protocol_resource.c"
    "sp_protocol/mqtt_protocol_status.c"
    "sp_protocol/mqtt_record_publish_port.c"
    "sp_protocol/sp_pro_proto.c"
)

set(APP_SOURCES
    "sp_pro_app/sp_pro_app.c"
    "sp_pro_app/builder/builder_audio.c"
    "sp_pro_app/builder/builder_cmd.c"
    "sp_pro_app/builder/builder_elem.c"
    "sp_pro_app/builder/builder_page.c"
    "sp_pro_app/input/input_key_scan.c"
    "sp_pro_app/state/state_alarm.c"
    "sp_pro_app/state/state_calibration.c"
    "sp_pro_app/state/state_clear_bean.c"
    "sp_pro_app/state/state_drink.c"
    "sp_pro_app/state/state_lock.c"
    "sp_pro_app/state/state_maint.c"
    "sp_pro_app/state/state_power.c"
    "sp_pro_app/state/state_ready.c"
    "sp_pro_app/state/state_setting.c"
    "sp_pro_app/state/state_ota.c"
    "sp_pro_app/state/state_wifi.c"
)

set(BLE_SOURCES
    "ble_GATT/ble_pairing.c"
    "ble_mesh/ble_mesh_client_drv.c"
    "ble_mesh/ble_mesh_scale_client_mod.c"
)

set(SRC_FILES
    ${APP_ROOT_SOURCES}
    ${BSP_DRIVER_SOURCES}
    ${MQTT_AUTH_SOURCES}
    ${OTA_SOURCES}
    ${SERVICE_SOURCES}
    ${PROTOCOL_SOURCES}
    ${APP_SOURCES}
    ${BLE_SOURCES}
)

idf_component_register(
    SRCS ${SRC_FILES}

    INCLUDE_DIRS
        "."
        "service"
        "ota_ctr_esp32"
        "mqtt_auth"
        "sp_protocol"
        "sp_pro_app"
        "bsp_driver"
        "ble_mesh"
        "ble_GATT"

    REQUIRES
        opus_player
        opus_player_test
        nvs_flash
        bt
        esp_wifi
        esp_event
        driver
        esp_netif
        app_update
        esp_https_ota
        mqtt
        esp_http_client
)

# Create the SPIFFS partition image from server/opus_files.
spiffs_create_partition_image(opus_data "../server/opus_files" FLASH_IN_PROJECT)
