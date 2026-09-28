# main 组件：硬件无关的系统初始化 + 选中项目的装配文件
#
# 「项目即宏」：与 Kconfig.projbuild 的 NODE_PROJECT choice 对应，一个项目宏
# 映射到一个 main/boards/<name>/ 目录。显式源文件列表（不用 file(GLOB)）。
# REQUIRES 无条件声明（ESP-IDF 早期依赖收集阶段读不到 CONFIG 变量），
# 只对 SRCS 做条件编译。

set(SRCS "main.cpp" "Application.cpp")

# 项目装配（与 main/Kconfig.projbuild 的 NODE_PROJECT choice 一一对应）
if(CONFIG_NODE_PROJECT_C3_OLED_THP)
    set(BOARD_DIR "boards/c3_oled_thp")
    set(BOARD_SRC "C3OledThpBoard.cpp")
elseif(CONFIG_NODE_PROJECT_C3_OLED_VOC)
    set(BOARD_DIR "boards/c3_oled_voc")
    set(BOARD_SRC "C3OledVocBoard.cpp")
elseif(CONFIG_NODE_PROJECT_S3_TFT_FAN)
    set(BOARD_DIR "boards/s3_tft_fan")
    set(BOARD_SRC "S3TftFanBoard.cpp")
endif()

if(BOARD_DIR)
    list(APPEND SRCS "${BOARD_DIR}/${BOARD_SRC}")
endif()

# 显示驱动与界面模板（main/display/，按 NODE_DISPLAY_* 条件编入；
# REQUIRES 已在下方无条件声明，此处只条件化 SRCS —— 早期依赖收集读不到 CONFIG_*）
if(CONFIG_NODE_DISPLAY_SSD1315)
    list(APPEND SRCS
        "display/Ssd1315Display.cpp"
        "display/DashboardScreen.cpp"
    )
endif()

if(CONFIG_NODE_DISPLAY_SSD1306_128X32)
    list(APPEND SRCS
        "display/Ssd1306Display.cpp"
        "display/CompactDashboardScreen.cpp"
    )
endif()

if(CONFIG_NODE_DISPLAY_ILI9341_240X320)
    list(APPEND SRCS
        "display/SpitftTouchDisplay.cpp"
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
set(BASE_REQUIRES
    app_config
    sensor_registry
    data_pipeline
    ble_peripheral
    power_manager
    i2c_bus
    hardware_context
    sensors
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

# 项目目录作为私有 include 路径：使该项目的 <Xxx>Board.cpp 可直接 #include "config.h"，
# 且 config.h 不泄漏给其他组件。早期依赖收集阶段 BOARD_DIR 可能为空，需保护。
if(BOARD_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${BOARD_DIR}")
endif()
