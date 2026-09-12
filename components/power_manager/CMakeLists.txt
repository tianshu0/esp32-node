# power_manager：低功耗管理（可选启用）
idf_component_register(
    SRCS
        "PowerManager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_config
        esp_pm
        esp_common
        log
)
