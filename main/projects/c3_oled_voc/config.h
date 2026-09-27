// 项目编译期配置：ESP32-C3 + SSD1315 OLED + 21VOC 五合一空气质量模块
//
// I2C 总线上只有 OLED（0x3C）；21VOC 模块独占 UART1（点对点，无需总线仲裁）。
// 实际 I2C 引脚可被 NVS 中的 app_config 覆盖，这里只放板级固定/默认常量。
//
// 本文件只被本项目的 Board.cpp 使用，组件层不直接 include（对标 esp32-xiaozhi
// 每个 board 目录自带 config.h 的机制）。
#pragma once

#include "driver/uart.h"

namespace esp32node::project_c3_oled_voc {

// I2C 默认引脚（与 AppConfig::kDefaultSda/kDefaultScl 一致）
inline constexpr int kI2cSda = 4;
inline constexpr int kI2cScl = 5;

// 21VOC 模块：UART1，TX->模块RX，RX<-模块TX
// 避开 strapping(2/8/9)、SPI flash(11~17)、USB(18/19)、UART0(20/21)
inline constexpr uart_port_t kVocUartPort = UART_NUM_1;
inline constexpr int kVocUartTx = 6;   // ESP32-C3 TX -> 21VOC RX
inline constexpr int kVocUartRx = 7;   // ESP32-C3 RX <- 21VOC TX

// OLED 模块无独立复位脚（-1 = 无复位脚）
inline constexpr int kOledResetGpio = -1;

} // namespace esp32node::project_c3_oled_voc
