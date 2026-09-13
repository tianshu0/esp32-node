# i2c_bus：I2C 主机总线封装（sensors 驱动 / display_service 共用）
idf_component_register(
    SRCS
        "I2cBus.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
        esp_common
        log
)
