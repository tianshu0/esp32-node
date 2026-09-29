idf_component_register(
    SRCS
        "automation.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fan_control sensor_registry app_config
             nvs_flash esp_timer log freertos
)
