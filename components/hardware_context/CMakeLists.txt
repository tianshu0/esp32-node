# hardware_context：纯头文件组件（总线句柄集合），传递依赖 i2c_bus / uart_bus 头文件
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES i2c_bus uart_bus
)
