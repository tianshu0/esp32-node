# ble_peripheral：NimBLE Peripheral（广播 + GATT 服务端 + 握手 + 数据上报）
idf_component_register(
    SRCS
        "BlePeripheral.cpp"
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
