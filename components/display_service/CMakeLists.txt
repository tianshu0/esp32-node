# display_service：显示驱动（什么屏）+ 内容模板（显示什么）
#
# 注意：REQUIRES 必须无条件声明——ESP-IDF 在 Kconfig 变量注入前的早期阶段就要收集
# 组件依赖；条件化 REQUIRES 会在全新 configure 时丢依赖。只对 SRCS 做条件编译。
# 因此即使选了 NODE_DISPLAY_NONE，LVGL 仍参与构建（驱动源文件不编入，死代码由
# 链接器回收），换取配置机制在全新克隆构建时的稳定性。
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

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        app_config
        sensor_registry
        ble_peripheral
        i2c_bus
        hardware_context
        esp_lcd
        esp_driver_i2c
        esp_common
        log
        lvgl
    PRIV_REQUIRES
        esp_lvgl_port
)
