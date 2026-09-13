# sensors：传感器插件集合。每个驱动一个类，按 Kconfig 条件编入，显式源文件列表。
set(SRCS)

if(CONFIG_NODE_SENSOR_SHT3X)
    list(APPEND SRCS "Sht3xSensor.cpp")
endif()

if(CONFIG_NODE_SENSOR_BMP180)
    list(APPEND SRCS "Bmp180Sensor.cpp")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        app_config
        hardware_context
        i2c_bus
        sensor_registry
        esp_driver_i2c
        esp_timer
        esp_common
        log
)
