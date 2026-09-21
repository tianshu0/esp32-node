idf_component_register(
    SRCS "FanControl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer log
)
