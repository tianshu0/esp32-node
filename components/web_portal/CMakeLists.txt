idf_component_register(
    SRCS "web_portal.cc"
    INCLUDE_DIRS "include"
    # 配网页面静态资源（EMBED_TXTFILES 以 null 结尾嵌入二进制）
    EMBED_TXTFILES "web/index.html" "web/style.css" "web/app.js"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server lwip json
)
