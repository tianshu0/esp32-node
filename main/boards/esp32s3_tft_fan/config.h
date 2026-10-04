// 板级编译期配置：ESP32-S3 N16R8 + ILI9341 240x320 SPI TFT + XPT2046 触摸
//                   + AHT30 温湿度模块 + PWM 风扇
//
// 模块：ESP32-S3-WROOM-1-N16R8（16MB OCTAL Flash + 8MB OCTAL PSRAM）
//   - OCTAL Flash 占用 GPIO33-40，OCTAL PSRAM 占用 GPIO41-47
//   - USB D-/D+ 占用 GPIO19/20（本项目不用 USB）
//
// 两组独立 SPI：
//   - SPI2_HOST 驱动 ILI9341 LCD（DMA）
//   - SPI3_HOST 驱动 XPT2046 触摸（无 DMA，独立 4 线，不与 LCD 并联）
// I2C0 挂 AHT30(0x38)，SDA/SCL。
//
// 只被本板 board_esp32s3_tft_fan.cc 使用，组件层不 include。引脚编号均为 GPIO 号。
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ================ ILI9341 LCD（SPI2_HOST）================
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIN_MOSI GPIO_NUM_11
#define LCD_PIN_MISO GPIO_NUM_10  // LCD 只读不用，可悬空
#define LCD_PIN_SCLK GPIO_NUM_12
#define LCD_PIN_CS   GPIO_NUM_13
#define LCD_PIN_DC   GPIO_NUM_14
#define LCD_PIN_RST  GPIO_NUM_21
#define LCD_PIN_BL   GPIO_NUM_47
#define LCD_PCLK_HZ  (20 * 1000 * 1000)

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
// 竖屏（Portrait）：panel/lvgl 同步 X 镜像
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false

// ================ XPT2046 触摸（SPI3_HOST，无 DMA）================
#define TOUCH_SPI_HOST SPI3_HOST
#define TOUCH_PIN_SCK  GPIO_NUM_15
#define TOUCH_PIN_MOSI GPIO_NUM_16
#define TOUCH_PIN_MISO GPIO_NUM_17
#define TOUCH_PIN_CS   GPIO_NUM_48

// ================ I2C0（AHT20 0x38 / BMP280 0x76|0x77）================
#define I2C_PIN_SDA GPIO_NUM_8
#define I2C_PIN_SCL GPIO_NUM_9

// ================ 风扇 PWM（LEDC，25kHz 标准四线风扇频率）================
#define FAN_PIN_PWM     GPIO_NUM_1
#define FAN_PWM_FREQ_HZ 25000
