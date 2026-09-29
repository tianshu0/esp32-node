// AHT20 温湿度传感器驱动（I2C）—— SensorDevice 插件实现
//
// - 7 位地址：0x38（固定）
// - 上电后等待 ≥40ms，发送校准命令 0xBE 0x08 0x00；状态字 bit3=1 表示已校准
// - 触发测量：0xAC 0x33 0x00，转换最长 80ms（状态字 bit7=0 表示完成）
// - 读回 7 字节：status + 20bit 湿度 + 20bit 温度 + CRC8（poly 0x31，覆盖前 6 字节）
//     RH = raw_h / 2^20 * 100 %；T = raw_t / 2^20 * 200 - 50 ℃
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "sensors/sensor_device.hpp"
#include "sensor_registry/sensor_registry.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class Aht20Sensor : public SensorDevice {
public:
    static constexpr uint8_t kAddr = 0x38;

    const char* Type() const override { return "temp_hum"; }
    esp_err_t Start(HardwareContext& hw, AppConfig& config,
                    SensorRegistry& registry) override;

    // 单次采集：温度（℃）+ 相对湿度（%RH）
    bool Read(float* temperature_c, float* humidity_pct);

    // 供 SensorRegistry 使用的统一签名
    static bool ReadThunk(void* ctx, SensorReading* out);

private:
    static constexpr uint32_t kI2cTimeoutMs = 100;

    esp_err_t Transmit(const uint8_t* data, size_t len);
    bool ReadStatus(uint8_t* status);

    i2c_master_dev_handle_t dev_ = nullptr;
};

} // namespace esp32node
