# wifi_portal：SoftAP + Captive Portal 网页配网组件
idf_component_register(
    SRCS
        "WifiPortal.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        lwip
        nvs_flash
        log
        app_config
        sensor_registry
        fan_control
)
