#pragma once

#include <driver/gpio.h>
#include <driver/uart.h>

// OLED（SSD1315）I2C 引脚
#define DISPLAY_SDA_PIN GPIO_NUM_4
#define DISPLAY_SCL_PIN GPIO_NUM_5

#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64

#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// 21VOC 模块：UART1，ESP TX -> 模块 RX，ESP RX <- 模块 TX
// 避开 strapping(2/8/9)、SPI flash(11~17)、USB(18/19)、UART0(20/21)
#define VOC_UART_PORT UART_NUM_1
#define VOC_UART_TX_PIN GPIO_NUM_6
#define VOC_UART_RX_PIN GPIO_NUM_7
