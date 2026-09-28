// c3_oled_thp 板：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
#pragma once

#include "boards/Board.hpp"

namespace esp32node {

class C3OledThpBoard : public Board {
public:
    using Board::Board;

    void Assemble() override;
    const char* Name() const override { return "c3_oled_thp"; }
};

} // namespace esp32node
