// sensor_driver 组件：传感器驱动聚合层
//
// 职责：
//   - 持有 I2C 总线（SHT3X 与 BMP180 共用）
//   - 逐个初始化传感器，并把能力登记到 sensor_registry
//   - 不参与采集调度：data_pipeline 通过 sensor_registry 遍历采集
//
// 单个传感器初始化失败不影响整机启动（记警告并跳过），
// 节点仍会带着可用传感器继续广播/握手。
#pragma once

#include "esp_err.h"
#include "sensor_driver/I2cBus.hpp"
#include "sensor_driver/Sht3x.hpp"
#include "sensor_driver/Bmp180.hpp"

namespace esp32node {

class AppConfig;
class SensorRegistry;

class SensorDriver {
public:
    SensorDriver() = default;
    ~SensorDriver() = default;

    // 初始化 I2C 总线与各传感器，并把能力登记到 registry
    esp_err_t Init(AppConfig* config, SensorRegistry* registry);

    bool Sht3xReady() const { return sht3x_ready_; }
    bool Bmp180Ready() const { return bmp180_ready_; }

private:
    I2cBus bus_;
    Sht3x sht3x_;
    Bmp180 bmp180_;
    bool sht3x_ready_ = false;
    bool bmp180_ready_ = false;
};

} // namespace esp32node
