# main 组件：硬件无关的系统初始化 + Kconfig 选中的板型装配文件
#
# 显式源文件列表（不用 file(GLOB)）。REQUIRES 无条件声明（ESP-IDF 早期依赖收集
# 阶段读不到 CONFIG 变量），只对 SRCS 做条件编译。

set(SRCS "main.cpp")

# 板型装配（与 main/Kconfig.projbuild 的 NODE_BOARD choice 对应）
if(CONFIG_NODE_BOARD_C3_I2C_OLED)
    list(APPEND SRCS "boards/c3_i2c_oled/Board.cpp")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "boards"
    REQUIRES
        app_config
        sensor_registry
        data_pipeline
        ble_peripheral
        power_manager
        i2c_bus
        hardware_context
        sensors
        display_service
        esp_event
        log
        bt
)
