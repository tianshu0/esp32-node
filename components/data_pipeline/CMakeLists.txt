# data_pipeline：定时采集 + 打包 + 事件投递
idf_component_register(
    SRCS
        "DataPipeline.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_config
        sensor_registry
        esp_event
        esp_common
        log
)
