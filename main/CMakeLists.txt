# main 组件：硬件无关的系统初始化 + 通用框架模块 + 选中项目的装配文件
#
# 「项目即宏」：与 Kconfig.projbuild 的 NODE_PROJECT choice 对应，一个项目宏
# 映射到一个 main/boards/<name>/ 目录。显式源文件列表（不用 file(GLOB)）。
# REQUIRES 无条件声明（ESP-IDF 早期依赖收集阶段读不到 CONFIG 变量），
# 只对 SRCS 做条件编译。
#
# main/common/：跨项目通用模块（原 components/<name>/，已折叠为 main 的普通源文件）。
#   app_config / sensor_registry / power_manager / fan_control / automation
#   为所有项目共用；link 为无线链路抽象（BleLink / WifiLink），由各项目板装配层择一实例化。
#   板载总线（I2C/UART）不单独成模块：接口声明在 boards/board.hpp，实现在各板
#   <Xxx>Board.cpp 里。
#   include 路径保持「模块/模块.hpp」，由 INCLUDE_DIRS 的 common 目录解析。

set(SRCS "main.cpp" "application.cpp")

# 通用框架模块（无条件编入，与原组件行为一致）
list(APPEND SRCS
    "common/app_config/app_config.cpp"
    "common/sensor_registry/sensor_registry.cpp"
    "common/power_manager/power_manager.cpp"
    "common/fan_control/fan_control.cpp"
    "common/automation/automation.cpp"
    "common/wifi/wifi.cpp"
    "common/link/wifi_link.cpp"
)

# BLE 外设链路：仅启用蓝牙的项目（C3 节点）编入，与 CONFIG_BT_ENABLED 对齐
if(CONFIG_BT_ENABLED)
    list(APPEND SRCS
        "common/ble/ble.cpp"
        "common/link/ble_link.cpp"
    )
endif()

# 传感器驱动（main/sensors/，由 Kconfig 的项目 select 锁定具体型号）
if(CONFIG_NODE_SENSOR_SHT3X)
    list(APPEND SRCS "sensors/sht3x_sensor.cpp")
endif()
if(CONFIG_NODE_SENSOR_AHT20)
    list(APPEND SRCS "sensors/aht20_sensor.cpp")
endif()
if(CONFIG_NODE_SENSOR_BMP180)
    list(APPEND SRCS "sensors/bmp180_sensor.cpp")
endif()
if(CONFIG_NODE_SENSOR_BMP280)
    list(APPEND SRCS "sensors/bmp280_sensor.cpp")
endif()
if(CONFIG_NODE_SENSOR_VOC21)
    list(APPEND SRCS "sensors/voc21_sensor.cpp")
endif()

# 项目装配（与 main/Kconfig.projbuild 的 NODE_PROJECT choice 一一对应）
if(CONFIG_NODE_PROJECT_C3_OLED_THP)
    set(BOARD_DIR "boards/c3_oled_thp")
    list(APPEND SRCS
        "boards/c3_oled_thp/c3_oled_thp_board.cpp"
        "boards/c3_oled_thp/thp_display.cpp"
    )
elseif(CONFIG_NODE_PROJECT_C3_OLED_VOC)
    set(BOARD_DIR "boards/c3_oled_voc")
    list(APPEND SRCS
        "boards/c3_oled_voc/c3_oled_voc_board.cpp"
        "boards/c3_oled_voc/voc_display.cpp"
    )
elseif(CONFIG_NODE_PROJECT_S3_TFT_FAN)
    set(BOARD_DIR "boards/s3_tft_fan")
    list(APPEND SRCS
        "boards/s3_tft_fan/s3_tft_fan_board.cpp"
        "boards/s3_tft_fan/fan_display.cpp"
    )
endif()

# 显示框架基类（所有项目必须：display/display.hpp / no_display.hpp 被 board.hpp 引用）
# 字体文件仅 ILI9341 项目需要（中文界面）
if(CONFIG_NODE_DISPLAY_ILI9341_240X320)
    list(APPEND SRCS
        "display/fonts/lv_font_zh8.c"
        "display/fonts/lv_font_zh10.c"
        "display/fonts/lv_font_zh12.c"
        "display/fonts/lv_font_zh14.c"
        "display/fonts/lv_font_zh16.c"
        "display/fonts/lv_font_zh24.c"
    )
endif()

# REQUIRES 在早期依赖收集阶段读不到 CONFIG_*，必须无条件声明。
# 原各组件 REQUIRES 的 IDF 依赖在此汇总（app_config/sensor_registry/... 自身已并入 main）。
set(BASE_REQUIRES
    driver
    esp_common
    esp_event
    esp_netif
    esp_timer
    esp_wifi
    esp_http_server
    esp_driver_i2c
    esp_driver_uart
    esp_pm
    nvs_flash
    lwip
    json
    freertos
    esp_lcd
    esp_lcd_touch
    esp_lcd_touch_xpt2046
    espressif__esp_lcd_ili9341
    lvgl
    log
    bt
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "common"
    REQUIRES ${BASE_REQUIRES}
    PRIV_REQUIRES esp_lvgl_port
    EMBED_FILES "common/wifi/web/index.html"
)

# 项目目录作为私有 include 路径：使该项目的 <xxx>_board.cpp 可直接 #include "config.h"，
# 且 config.h 不泄漏给其他组件。早期依赖收集阶段 BOARD_DIR 可能为空，需保护。
if(BOARD_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${BOARD_DIR}")
endif()
