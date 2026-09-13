// 传感器驱动统一基类（插件机制）
//
// 每个传感器一个子类，职责：
//   1. Start() 内从 HardwareContext 取所需总线、初始化芯片；
//   2. 把能力（type/model/format/显示字段/采集函数）登记到 SensorRegistry；
//   3. 初始化失败返回错误码，由板型装配层决定告警跳过（不拖垮整机）。
//
// data_pipeline / ble_peripheral / 显示模板只依赖 SensorRegistry，
// 因此新增传感器不需要改动它们，也不需要改 main.cpp。
#pragma once

#include "esp_err.h"
#include "app_config/AppConfig.hpp"
#include "hardware_context/HardwareContext.hpp"
#include "sensor_registry/SensorRegistry.hpp"

namespace esp32node {

class SensorDevice {
public:
    virtual ~SensorDevice() = default;

    // 传感器类型标识，如 "temp_hum"（与登记到 registry 的 type 一致）
    virtual const char* Type() const = 0;

    // 初始化硬件并把能力登记到 registry。
    // 硬件不存在/应答失败时返回非 OK，调用方告警并跳过。
    virtual esp_err_t Start(HardwareContext& hw, AppConfig& config,
                            SensorRegistry& registry) = 0;
};

} // namespace esp32node
