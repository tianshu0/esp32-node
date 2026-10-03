// s3_tft_fan 板：ESP32-S3 N16R8 + ILI9341 240x320 SPI TFT + XPT2046 触摸
//                + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
//
// 构造函数：创建 FanDisplay（面板/LVGL/触摸在构造内初始化）
// Assemble()：建 SPI/I2C 总线 -> 实例化传感器/风扇/自动化
#pragma once

#include <cstddef>
#include <cstdint>
#include "boards/board.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class S3TftFanBoard : public Board {
public:
    explicit S3TftFanBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "s3_tft_fan"; }

    // 传感器数据出口
    int ReadSensors(SensorReading* out, int max) override;
    const char* SensorTypesJson() const override { return types_json_; }
    const char* SensorDetailJson() const override { return detail_json_; }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;

    // BLE/WiFi 握手能力清单
    char types_json_[128] = "[]";
    char detail_json_[512] = "[]";
};

} // namespace esp32node
