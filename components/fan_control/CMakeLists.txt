idf_component_register(
    SRCS
        "fan_control.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer log
)
