# sensor_driver：I2C 总线 + SHT3X / BMP180 传感器驱动
idf_component_register(
    SRCS
        "I2cBus.cpp"
        "Sht3x.cpp"
        "Bmp180.cpp"
        "SensorDriver.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_config
        sensor_registry
        esp_driver_i2c
        esp_timer
        esp_common
        log
)
