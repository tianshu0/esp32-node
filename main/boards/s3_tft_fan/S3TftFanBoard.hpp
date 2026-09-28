// s3_tft_fan 板：ESP32-S3 N16R8 + ILI9341 240x320 SPI TFT + XPT2046 触摸
//                + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
#pragma once

#include "boards/Board.hpp"

namespace esp32node {

class S3TftFanBoard : public Board {
public:
    using Board::Board;

    void Assemble() override;
    const char* Name() const override { return "s3_tft_fan"; }
};

} // namespace esp32node
