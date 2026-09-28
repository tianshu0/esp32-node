# wifi_portal：SoftAP + Captive Portal 网页配网组件
# EMBED_FILES：web/index.html 编译期嵌入固件只读段（无需 SPIFFS 分区），
# 生成符号 _binary_index_html_start / _binary_index_html_end（只取 basename）
idf_component_register(
    SRCS
        "WifiPortal.cpp"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "web/index.html"
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
