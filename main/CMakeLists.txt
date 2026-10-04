# 公共源文件（与板型无关）
set(SOURCES
    "main.cc"
    "application.cc"
    "display/display.cc"
    "boards/common/board.cc"
)

set(INCLUDE_DIRS "." "display" "boards/common")

# 注意：组件依赖必须无条件列出——requirements 预扫描阶段没有 sdkconfig，
# 条件式 PRIV_REQUIRES 不会建立 include 传播（本地组件始终全量编译，
# 未被引用的归档符号不会进固件，因此列出 sht3x 不会增加其他板型的体积）。
set(PRIV_REQUIRES_LIST
    nvs_flash
    esp_lcd
    esp_driver_i2c
    esp_driver_uart
    esp_driver_spi
    esp_driver_gpio
    esp_driver_ledc
    lvgl
    esp_lvgl_port
    waveshare__esp_oled_ssd1315
    espressif__esp_lcd_ili9341
    espressif__esp_lcd_touch
    atanisoft__esp_lcd_touch_xpt2046
    sht3x
    voc21
    aht30
    fonts
)

# 板型选择（Kconfig: Node project -> Board type）
# 每块板显式列出自己的板级源文件与显示驱动，不使用 GLOB。
# 注意：本文件在 Kconfig 加载前的组件 requirements 预扫描阶段也会被解析，
# 届时 CONFIG_BOARD_* 均为空属正常，因此 idf_component_register 必须无条件执行，
# 板型分支只能用于追加源文件，不能 FATAL_ERROR。
if(CONFIG_BOARD_ESP32C3_SHT3X)
    list(APPEND SOURCES
        "boards/esp32c3_sht3x/board_esp32c3_sht3x.cc"
        "display/ssd1315_oled_display.cc"
    )
    list(APPEND INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32c3_sht3x")
elseif(CONFIG_BOARD_ESP32C3_VOC21)
    list(APPEND SOURCES
        "boards/esp32c3_voc21/board_esp32c3_voc21.cc"
        "display/ssd1315_oled_display.cc"
    )
    list(APPEND INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32c3_voc21")
elseif(CONFIG_BOARD_ESP32S3_TFT_FAN)
    list(APPEND SOURCES
        "boards/esp32s3_tft_fan/board_esp32s3_tft_fan.cc"
        "display/ili9341_tft_display.cc"
    )
    list(APPEND INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3_tft_fan")
endif()

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES ${PRIV_REQUIRES_LIST}
                    )
