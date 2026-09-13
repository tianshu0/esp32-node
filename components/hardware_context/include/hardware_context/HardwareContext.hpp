// hardware_context 组件：板级总线/外设句柄的统一集合
//
// 传感器驱动不自己创建总线（避免一条物理总线上挂多个主机），而是由板型装配层
// 创建后通过 HardwareContext 传入。当前只有 I2C；将来加 SPI/ADC/1-Wire 传感器时
// 在这里扩展指针即可，所有传感器 Start() 签名保持不变。
//
// 生命周期契约：传感器驱动只在 Start() 内使用本结构（取句柄后不存指针），
// 但显示驱动会把指向本结构的指针存入成员长期使用——因此装配层的 HardwareContext
// 实例必须 static 常驻，不能放在栈上。
//
// 纯头文件组件，无源码、无任务。
#pragma once

#include "i2c_bus/I2cBus.hpp"

namespace esp32node {

struct HardwareContext {
    I2cBus* i2c = nullptr;
    // 将来扩展：SpiBus* spi = nullptr; 等
};

} // namespace esp32node
