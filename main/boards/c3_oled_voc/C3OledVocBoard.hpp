// c3_oled_voc 板：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
#pragma once

#include "boards/Board.hpp"

namespace esp32node {

class C3OledVocBoard : public Board {
public:
    using Board::Board;

    void Assemble() override;
    const char* Name() const override { return "c3_oled_voc"; }
};

} // namespace esp32node
