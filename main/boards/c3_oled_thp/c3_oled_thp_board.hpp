// c3_oled_thp 板：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
//
// 构造函数：探测 SSD1315 面板，成功则创建 ThpDisplay，失败则 NoDisplay
// Assemble()：建 I2C 总线 -> 实例化传感器（面板已在构造时创建）
#pragma once

#include "boards/Board.hpp"

namespace esp32node {

class C3OledThpBoard : public Board {
public:
    explicit C3OledThpBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "c3_oled_thp"; }
};

} // namespace esp32node
