// SHT3X（SHT30）温湿度传感器驱动（I2C）
//
// - 7 位地址：0x44（ADDR 接低，模块默认）/ 0x45（ADDR 接高）
// - 单次测量、高重复度、不启用时钟拉伸（命令 0x2400），量程：
//     温度 -45 ~ +125 ℃，湿度 0 ~ 100 %RH
// - 每 2 字节数据后跟 1 字节 CRC-8（多项式 0x31，初值 0xFF）
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "sensor_registry/SensorRegistry.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class I2cBus;

class Sht3x {
public:
    static constexpr uint8_t kAddrDefault = 0x44;
    static constexpr uint8_t kAddrAlt = 0x45;

    Sht3x() = default;

    // 挂载设备并软复位到已知状态
    esp_err_t Init(I2cBus& bus, uint8_t addr = kAddrDefault);

    // 单次采集：温度（℃）+ 相对湿度（%RH）
    bool Read(float* temperature_c, float* humidity_pct);

    // 供 SensorRegistry 使用的统一签名
    static bool ReadThunk(void* ctx, SensorReading* out);

private:
    esp_err_t SendCommand(uint16_t cmd);

    i2c_master_dev_handle_t dev_ = nullptr;
};

} // namespace esp32node
