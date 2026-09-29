# sensor_registry：节点自身传感器能力注册表（纯数据，被动组件）
idf_component_register(
    SRCS
        "sensor_registry.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        log
)
