# main 组件：硬件无关的系统初始化 + 选中项目的装配文件
#
# 「项目即宏」：与 Kconfig.projbuild 的 NODE_PROJECT choice 对应，一个项目宏
# 映射到一个 main/boards/<name>/ 目录。显式源文件列表（不用 file(GLOB)）。
# REQUIRES 无条件声明（ESP-IDF 早期依赖收集阶段读不到 CONFIG 变量），
# 只对 SRCS 做条件编译。

set(SRCS "main.cpp" "application.cpp")

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
# 字体文件仅 s3_tft_fan 需要（ILI9341 项目）
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

# 基础组件（所有项目必须）
# 注意：REQUIRES 在早期依赖收集阶段读不到 CONFIG_*，必须无条件声明；
# fan_control / automation 本身是可移植 C++ 组件，在 C3 上也能正常编译，
# 仅 s3_tft_fan 项目的 Board.cpp 会实际引用它们（SRCS 按项目条件编入）。
# sensors / hardware_context 已迁入 main/（仅被板级装配引用，无组件层依赖方）。
set(BASE_REQUIRES
    app_config
    sensor_registry
    ble_peripheral
    power_manager
    i2c_bus
    uart_bus
    fan_control
    automation
    wifi_portal
    esp_lcd
    esp_lcd_touch
    esp_lcd_touch_xpt2046
    espressif__esp_lcd_ili9341
    driver
    esp_common
    esp_netif
    lvgl
    esp_event
    log
    bt
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${BASE_REQUIRES}
    PRIV_REQUIRES esp_lvgl_port
)

# 项目目录作为私有 include 路径：使该项目的 <xxx>_board.cpp 可直接 #include "config.h"，
# 且 config.h 不泄漏给其他组件。早期依赖收集阶段 BOARD_DIR 可能为空，需保护。
if(BOARD_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${BOARD_DIR}")
endif()
