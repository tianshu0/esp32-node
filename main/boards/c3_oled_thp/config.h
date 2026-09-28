// 项目编译期配置：ESP32-C3 + SSD1315 OLED + SHT3X/BMP180 温湿度气压节点
//
// I2C 设备（SHT3X 0x44 / BMP180 0x77 / OLED 0x3C）共用一条总线。
// 实际 I2C 引脚可被 NVS 中的 app_config 覆盖，这里只放板级固定/默认常量。
//
// 本文件只被本项目的 Board.cpp 使用，组件层不直接 include（对标 esp32-xiaozhi
// 每个 board 目录自带 config.h 的机制）。
#pragma once

namespace esp32node::project_c3_oled_thp {

// I2C 默认引脚（与 AppConfig::kDefaultSda/kDefaultScl 一致）
inline constexpr int kI2cSda = 4;
inline constexpr int kI2cScl = 5;

// OLED 模块无独立复位脚（-1 = 无复位脚）
inline constexpr int kOledResetGpio = -1;

} // namespace esp32node::project_c3_oled_thp
