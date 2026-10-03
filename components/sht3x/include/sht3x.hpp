// SHT3X（SHT30）温湿度传感器驱动（I2C）—— 独立可移植组件
//
// 仅依赖 ESP-IDF：driver/i2c_master.h
// 板级负责创建 i2c_master_bus_handle_t 并传入；本组件自行挂载设备句柄。
//
// - 7 位地址：0x44（ADDR 接低，模块默认）/ 0x45（ADDR 接高）
// - 单次测量、高重复度、不启用时钟拉伸（命令 0x2400），量程：
//     温度 -45 ~ +125 ℃，湿度 0 ~ 100 %RH
// - 每 2 字节数据后跟 1 字节 CRC-8（多项式 0x31，初值 0xFF）
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/i2c_master.h"

namespace esp32node {

class Sht3x {
public:
    static constexpr uint8_t kAddrDefault = 0x44;
    static constexpr uint8_t kAddrAlt = 0x45;

    Sht3x(i2c_master_bus_handle_t bus, uint8_t addr = kAddrDefault);
    ~Sht3x();

    // 软复位（同时作为在线自检：不应答即视为未接）。
    esp_err_t Init();

    // 单次采集：温度（℃）+ 相对湿度（%RH）。任一指针可传 nullptr 跳过。
    bool Read(float* temperature_c, float* humidity_pct);

private:
    esp_err_t SendCommand(uint16_t cmd);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t addr_ = kAddrDefault;
};

} // namespace esp32node
