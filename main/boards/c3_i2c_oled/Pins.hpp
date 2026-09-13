// 板型引脚定义：ESP32-C3 + I2C 传感器组 + SSD1315 OLED
//
// 所有 I2C 设备（SHT3X 0x44 / BMP180 0x77 / OLED 0x3C）共用一条总线。
// 实际引脚可被 NVS 中的 app_config 覆盖，这里只放板级固定/默认常量。
#pragma once

namespace esp32node::board_c3_i2c_oled {

// I2C 默认引脚（与 AppConfig::kDefaultSda/kDefaultScl 一致，
// 板级装配实际取 config->I2cSda()/I2cScl()，此处仅作文档与未来板级专用脚用途）
inline constexpr int kI2cSda = 4;
inline constexpr int kI2cScl = 5;

// SSD1315 模块无独立复位脚
inline constexpr int kOledResetGpio = -1;

} // namespace esp32node::board_c3_i2c_oled
