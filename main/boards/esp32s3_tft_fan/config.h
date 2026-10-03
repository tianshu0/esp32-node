// 项目编译期配置：ESP32-S3 N16R8 + SPI TFT + XPT2046 触摸 + AHT20/BMP280 + PWM 风扇
//
// 模块：ESP32-S3-WROOM-1-N16R8（16MB OCTAL Flash + 8MB OCTAL PSRAM）
//   - OCTAL Flash 占用 GPIO33-40，OCTAL PSRAM 占用 GPIO41-47
//   - USB D-/D+ 占用 GPIO19/20（本项目不用 USB）
//   - Strapping: GPIO0(BOOT), GPIO3, GPIO45, GPIO46 — 启动后可用
//
// 本板使用两组独立 SPI：
//   - SPI2_HOST 驱动 ILI9341 LCD（DMA）
//   - SPI3_HOST 驱动 XPT2046 触摸（无 DMA，独立 4 线，不与 LCD 并联）
// I2C 总线挂 AHT20(0x38) + BMP280(0x76) 二合一模块，共用 SDA/SCL。
//
// 本文件只被本项目的 Board.cpp 使用，组件层不直接 include（对标 esp32-xiaozhi
// 每个 board 目录自带 config.h 的机制）。所有引脚编号为 GPIO 号。
#pragma once

#include "driver/spi_master.h"

namespace esp32node::project_s3_tft_fan {

// ================ SPI2_HOST（ILI9341 LCD 专用）================
// ILI9341 2.4" 240x320 屏幕
inline constexpr int kSpiMosi = 11;    // SPI MOSI (SDA)
inline constexpr int kSpiMiso = 10;    // LCD MISO（LCD 只读不用，可悬空）
inline constexpr int kSpiSclk = 12;    // SPI SCLK
inline constexpr int kLcdCs   = 13;    // LCD 片选
inline constexpr int kLcdDc   = 14;    // LCD 命令/数据选择
inline constexpr int kLcdRst  = 21;    // LCD 复位（-1 表示软复位）
inline constexpr int kLcdBl   = 47;    // LCD 背光（-1 表示接 3V3）

// ================ SPI3_HOST（XPT2046 触摸专用，独立 4 线）================
// 不与 LCD 共享总线：省掉杜邦线并联，LCD 的 MISO(GPIO10) 可完全不接。
// 无 DMA：XPT2046 单次事务仅 3 字节，轮询传输即可。
inline constexpr spi_host_device_t kTouchHost = SPI3_HOST;
inline constexpr int kTouchSck  = 15;   // T_CLK
inline constexpr int kTouchMosi = 16;   // T_DIN
inline constexpr int kTouchMiso = 17;   // T_DO
inline constexpr int kTouchCs   = 48;   // T_CS
inline constexpr int kTouchIrq  = -1;   // 触摸中断（-1 = 轮询模式，T_IRQ 不接）

// ================ I2C 总线（AHT20 0x38 / BMP280 0x76 二合一模块共用）================
inline constexpr int kI2cSda = 8;
inline constexpr int kI2cScl = 9;

// ================ 风扇 PWM（LEDC 通道，1-25kHz 可调频）================
inline constexpr int kFanPwmGpio = 1;    // 风扇 PWM 输出（接 4 线风扇的 PWM 脚）
inline constexpr int kFanPwmFreq = 25000; // 25kHz 标准 PWM 风扇频率
inline constexpr int kFanPwmRes  = 8;     // 8-bit 分辨率 (0-255)

// 风扇额外使能脚（可选，-1 表示不需要）
inline constexpr int kFanEnGpio = -1;

} // namespace esp32node::project_s3_tft_fan
