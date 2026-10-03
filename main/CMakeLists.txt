# 公共源文件（与板型无关）
set(SOURCES
    "main.cc"
    "application.cc"
    "display/display.cc"
    "boards/common/board.cc"
)

set(INCLUDE_DIRS "." "display" "boards/common")

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
    message(WARNING "BOARD_ESP32C3_VOC21 尚未迁移到新骨架")
elseif(CONFIG_BOARD_ESP32S3_TFT_FAN)
    message(WARNING "BOARD_ESP32S3_TFT_FAN 尚未迁移到新骨架")
endif()

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        nvs_flash
                        esp_lcd
                        esp_driver_i2c
                        lvgl
                        esp_lvgl_port
                        waveshare__esp_oled_ssd1315
                    )
