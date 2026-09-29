# i2c_bus：I2C 主机总线封装（传感器驱动 / 板级显示共用）
idf_component_register(
    SRCS
        "i2c_bus.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
        esp_common
        log
)
