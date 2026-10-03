// c3_oled_thp 板：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
//
// Assemble()：建 I2C 总线 -> 实例化传感器组件 -> 探测面板地址并创建 ThpDisplay
#pragma once

#include <cstddef>
#include <cstdint>
#include "boards/board.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class C3OledThpBoard : public Board {
public:
    explicit C3OledThpBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "c3_oled_thp"; }

    // OLED 地址探测（传感器组件自行挂载 I2C 设备，不再经 Board 访问总线）
    bool I2cProbe(uint8_t addr) override;

    // 传感器数据出口
    int ReadSensors(SensorReading* out, int max) override;
    const char* SensorTypesJson() const override { return types_json_; }
    const char* SensorDetailJson() const override { return detail_json_; }

private:
    static constexpr int kI2cTimeoutMs = 100;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;

    // BLE 握手能力清单（Assemble 内按传感器初始化结果构建）
    char types_json_[128] = "[]";
    char detail_json_[512] = "[]";
};

} // namespace esp32node
