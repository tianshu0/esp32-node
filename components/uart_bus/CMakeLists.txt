# uart_bus：UART 主机总线封装（21VOC 等串口传感器使用）
idf_component_register(
    SRCS
        "UartBus.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_uart
        esp_common
        log
)
