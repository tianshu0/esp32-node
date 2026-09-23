# main 组件：硬件无关的系统初始化 + Kconfig 选中的板型装配文件
#
# 显式源文件列表（不用 file(GLOB)）。REQUIRES 无条件声明（ESP-IDF 早期依赖收集
# 阶段读不到 CONFIG 变量），只对 SRCS 做条件编译。

set(SRCS "main.cpp")

# 板型装配（与 main/Kconfig.projbuild 的 NODE_BOARD choice 对应）
if(CONFIG_NODE_BOARD_C3_I2C_OLED)
    list(APPEND SRCS "boards/c3_i2c_oled/Board.cpp")
endif()

if(CONFIG_NODE_BOARD_S3_SPI_TFT)
    list(APPEND SRCS "boards/s3_spi_tft/Board.cpp")
endif()

# 基础组件（所有板型必须）
# 注意：REQUIRES 在早期依赖收集阶段读不到 CONFIG_*，必须无条件声明；
# fan_control / automation 本身是可移植 C++ 组件，在 C3 上也能正常编译，
# 仅 S3 板型的 Board.cpp 会实际引用它们（SRCS 仍按 CONFIG 条件编入）。
set(BASE_REQUIRES
    app_config
    sensor_registry
    data_pipeline
    ble_peripheral
    power_manager
    i2c_bus
    hardware_context
    sensors
    display_service
    fan_control
    automation
    wifi_portal
    esp_event
    log
    bt
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "boards"
    REQUIRES ${BASE_REQUIRES}
)
