# main 组件：硬件无关的系统初始化 + 选中项目的装配文件
#
# 「项目即宏」：与 Kconfig.projbuild 的 NODE_PROJECT choice 对应，一个项目宏
# 映射到一个 main/projects/<name>/ 目录。显式源文件列表（不用 file(GLOB)）。
# REQUIRES 无条件声明（ESP-IDF 早期依赖收集阶段读不到 CONFIG 变量），
# 只对 SRCS 做条件编译。

set(SRCS "main.cpp")

# 项目装配（与 main/Kconfig.projbuild 的 NODE_PROJECT choice 一一对应）
if(CONFIG_NODE_PROJECT_C3_OLED_THP)
    set(PROJECT_DIR "projects/c3_oled_thp")
elseif(CONFIG_NODE_PROJECT_C3_OLED_VOC)
    set(PROJECT_DIR "projects/c3_oled_voc")
elseif(CONFIG_NODE_PROJECT_S3_TFT_FAN)
    set(PROJECT_DIR "projects/s3_tft_fan")
endif()

if(PROJECT_DIR)
    list(APPEND SRCS "${PROJECT_DIR}/Board.cpp")
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
    INCLUDE_DIRS "."
    REQUIRES ${BASE_REQUIRES}
)

# 项目目录作为私有 include 路径：使该项目的 Board.cpp 可直接 #include "config.h"，
# 且 config.h 不泄漏给其他组件。早期依赖收集阶段 PROJECT_DIR 可能为空，需保护。
if(PROJECT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${PROJECT_DIR}")
endif()
