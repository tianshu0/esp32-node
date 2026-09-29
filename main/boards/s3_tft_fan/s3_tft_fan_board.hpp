// s3_tft_fan 板：ESP32-S3 N16R8 + ILI9341 240x320 SPI TFT + XPT2046 触摸
//                + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
//
// 构造函数：创建 FanDisplay（面板/LVGL/触摸在构造内初始化）
// Assemble()：建 SPI/I2C 总线 -> 实例化传感器/风扇/自动化
#pragma once

#include "boards/Board.hpp"

namespace esp32node {

class S3TftFanBoard : public Board {
public:
    explicit S3TftFanBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "s3_tft_fan"; }
};

} // namespace esp32node
