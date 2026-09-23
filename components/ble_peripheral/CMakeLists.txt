# ble_peripheral：NimBLE Peripheral（广播 + GATT 服务端 + 握手 + 数据上报）
#
# 当 BT 未启用时（如 s3_spi_tft 仅用 WiFi 配网），不编译任何源文件，
# 但组件仍存在（INCLUDE_DIRS 可用），让 display_service 等依赖方保持 REQUIRES 不变。
set(SRCS)
if(CONFIG_BT_ENABLED)
    list(APPEND SRCS "BlePeripheral.cpp")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_config
        sensor_registry
        data_pipeline
        bt
        esp_event
        json
        esp_common
        log
)
