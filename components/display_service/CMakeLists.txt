# display_service：显示驱动（什么屏）+ 内容模板（显示什么）
#
# 注意：REQUIRES 必须无条件声明——ESP-IDF 在 Kconfig 变量注入前的早期阶段就要收集
# 组件依赖；条件化 REQUIRES 会在全新 configure 时丢依赖。只对 SRCS 做条件编译。
set(SRCS)

if(CONFIG_NODE_DISPLAY_SSD1315)
    list(APPEND SRCS
        "Ssd1315Display.cpp"
        "DashboardScreen.cpp"
    )
endif()

if(CONFIG_NODE_DISPLAY_SSD1306_128X32)
    list(APPEND SRCS
        "Ssd1306Display.cpp"
        "CompactDashboardScreen.cpp"
    )
endif()

if(CONFIG_NODE_DISPLAY_ILI9341_240X320)
    list(APPEND SRCS
        "esp_lcd_ili9341.c"
        "SpitftTouchDisplay.cpp"
        "fonts/lv_font_zh14.c"
        "fonts/lv_font_zh16.c"
        "fonts/lv_font_zh24.c"
    )
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        app_config
        sensor_registry
        ble_peripheral
        i2c_bus
        hardware_context
        fan_control
        esp_lcd
        esp_lcd_touch
        esp_lcd_touch_xpt2046
        driver
        esp_common
        esp_netif
        log
        lvgl
    PRIV_REQUIRES
        esp_lvgl_port
)
