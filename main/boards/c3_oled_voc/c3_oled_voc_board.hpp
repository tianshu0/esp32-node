// c3_oled_voc 板：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
//
// 构造函数：探测 SSD1315 面板，成功则创建 VocDisplay，失败则 NoDisplay
// Assemble()：建 I2C/UART 总线 -> 实例化传感器（面板已在构造时创建）
#pragma once

#include "boards/Board.hpp"

namespace esp32node {

class C3OledVocBoard : public Board {
public:
    explicit C3OledVocBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "c3_oled_voc"; }
};

} // namespace esp32node
