# sensor_driver：SHT3X / BMP180 传感器驱动（I2C 总线由 i2c_bus 组件提供）
idf_component_register(
    SRCS
        "Sht3x.cpp"
        "Bmp180.cpp"
        "SensorDriver.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_config
        sensor_registry
        i2c_bus
        esp_driver_i2c
        esp_timer
        esp_common
        log
)
