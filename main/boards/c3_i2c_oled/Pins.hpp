// 板型引脚定义：ESP32-C3 + I2C 传感器组 + SSD1315/SSD1306 OLED + UART 传感器
//
// I2C 设备（SHT3X 0x44 / BMP180 0x77 / OLED 0x3C）共用一条总线；
// 21VOC 空气质量模块独占 UART1（点对点，无需总线仲裁）。
// 实际 I2C 引脚可被 NVS 中的 app_config 覆盖，这里只放板级固定/默认常量。
#pragma once

#include "driver/uart.h"

namespace esp32node::board_c3_i2c_oled {

// I2C 默认引脚（与 AppConfig::kDefaultSda/kDefaultScl 一致）
inline constexpr int kI2cSda = 4;
inline constexpr int kI2cScl = 5;

// 21VOC 模块：UART1，TX->模块RX，RX<-模块TX
// 避开 strapping(2/8/9)、SPI flash(11~17)、USB(18/19)、UART0(20/21)
inline constexpr uart_port_t kVocUartPort = UART_NUM_1;
inline constexpr int kVocUartTx = 6;   // ESP32-C3 TX -> 21VOC RX
inline constexpr int kVocUartRx = 7;   // 21VOC TX -> ESP32-C3 RX

// OLED 模块无独立复位脚
inline constexpr int kOledResetGpio = -1;

} // namespace esp32node::board_c3_i2c_oled
