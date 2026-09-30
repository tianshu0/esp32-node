// c3_oled_voc 板：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
//
// Assemble()：建 I2C/UART 总线 -> 实例化传感器 -> 探测面板地址并创建 VocDisplay
#pragma once

#include <cstddef>
#include <cstdint>
#include "boards/board.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class C3OledVocBoard : public Board {
public:
    explicit C3OledVocBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "c3_oled_voc"; }

    // I2C 总线上只有 OLED（由 esp_lcd panel IO 直接访问），故只需地址探测
    bool I2cProbe(uint8_t addr) override;

    // 21VOC 独占 UART1（点对点，无需总线仲裁）
    int UartRead(uint8_t* buf, size_t len, uint32_t timeout_ms) override;
    int UartWrite(const uint8_t* buf, size_t len) override;

private:
    static constexpr int kI2cTimeoutMs = 100;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
};

} // namespace esp32node
