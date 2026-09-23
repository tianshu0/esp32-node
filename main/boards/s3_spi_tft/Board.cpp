// 板型装配实现：ESP32-S3 N16R8 + SPI TFT (ILI9341) + XPT2046 触摸
//              + I2C 传感器 (SHT3X/BMP180) + UART 传感器 (21VOC) + PWM 风扇
//
// 硬件连接见同目录 Pins.hpp；本文件负责：
//   1. SPI2_HOST 总线（LCD + 触摸共享）
//   2. I2C 总线（SHT3X + BMP180）
//   3. UART1 总线（21VOC 空气质量模块）
//   4. LEDC PWM 风扇控制
//   5. SPI TFT 显示驱动（ILI9341 + XPT2046 触摸 + LVGL 多页 UI）
//   6. 自动化规则引擎（阈值触发 + 定时时长 + 迟滞保护）
//
// 与 c3_i2c_oled 板型的区别：
//   - SPI TFT + 触摸替代 I2C OLED
//   - 新增 FanControl + Automation 组件
//   - LVGL UI 从单屏刷新变为多页面触摸交互
#include "Board.hpp"
#include "s3_spi_tft/Pins.hpp"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "uart_bus/UartBus.hpp"
#include "hardware_context/HardwareContext.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#if CONFIG_NODE_SENSOR_SHT3X
#include "sensors/Sht3xSensor.hpp"
#endif
#if CONFIG_NODE_SENSOR_BMP180
#include "sensors/Bmp180Sensor.hpp"
#endif
#if CONFIG_NODE_SENSOR_VOC21
#include "sensors/Voc21Sensor.hpp"
#endif

#include "fan_control/FanControl.hpp"
#include "automation/Automation.hpp"
#include "display_service/SpitftTouchDisplay.hpp"

namespace esp32node {

static const char* TAG = "board-s3";

// SPI2_HOST 总线：ILI9341 LCD + XPT2046 触摸共享
// max_transfer_sz 按最大分区缓冲：横屏 320*20*2=12800B（竖屏 240*20*2=9600B）
static constexpr int kSpiMaxTransferBytes = 320 * 2 * 20;

void BoardAssemble(NodeContext& c)
{
    // ==================== 1. SPI2_HOST 总线 ====================
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = board_s3_spi_tft::kSpiMosi;
    buscfg.miso_io_num = board_s3_spi_tft::kSpiMiso;
    buscfg.sclk_io_num = board_s3_spi_tft::kSpiSclk;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = kSpiMaxTransferBytes;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI2(LCD) initialized: MOSI=%d MISO=%d SCLK=%d",
             board_s3_spi_tft::kSpiMosi,
             board_s3_spi_tft::kSpiMiso,
             board_s3_spi_tft::kSpiSclk);

    // ==================== 1b. SPI3_HOST 触摸专用总线（无 DMA）====================
    spi_bus_config_t touch_buscfg = {};
    touch_buscfg.mosi_io_num = board_s3_spi_tft::kTouchMosi;
    touch_buscfg.miso_io_num = board_s3_spi_tft::kTouchMiso;
    touch_buscfg.sclk_io_num = board_s3_spi_tft::kTouchSck;
    touch_buscfg.quadwp_io_num = -1;
    touch_buscfg.quadhd_io_num = -1;
    touch_buscfg.max_transfer_sz = 32;  // XPT2046 单次仅 3 字节
    ESP_ERROR_CHECK(spi_bus_initialize(board_s3_spi_tft::kTouchHost,
                                       &touch_buscfg, SPI_DMA_DISABLED));
    ESP_LOGI(TAG, "SPI3(touch) initialized: SCK=%d MOSI=%d MISO=%d CS=%d",
             board_s3_spi_tft::kTouchSck,
             board_s3_spi_tft::kTouchMosi,
             board_s3_spi_tft::kTouchMiso,
             board_s3_spi_tft::kTouchCs);

    // ==================== 2. I2C 总线 ====================
    static I2cBus i2c;
    ESP_ERROR_CHECK(i2c.Init(c.config->I2cSda(), c.config->I2cScl()));

    // ==================== 3. UART1 总线（21VOC）================
#if CONFIG_NODE_SENSOR_VOC21
    static UartBus uart;
    ESP_ERROR_CHECK(uart.Init(board_s3_spi_tft::kVocUartPort,
                              board_s3_spi_tft::kVocUartTx,
                              board_s3_spi_tft::kVocUartRx));
#endif

    // ==================== 4. 硬件上下文 ====================
    static HardwareContext hw;
    hw.i2c = &i2c;
#if CONFIG_NODE_SENSOR_VOC21
    hw.uart = &uart;
#endif

    // ==================== 5. 传感器 ====================
#if CONFIG_NODE_SENSOR_SHT3X
    static Sht3xSensor sht3x;
    if (sht3x.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "sht3x disabled, check wiring (addr 0x44/0x45)");
    }
#endif

#if CONFIG_NODE_SENSOR_BMP180
    static Bmp180Sensor bmp180;
    if (bmp180.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "bmp180 disabled, check wiring (addr 0x77)");
    }
#endif

#if CONFIG_NODE_SENSOR_VOC21
    static Voc21Sensor voc21;
    if (voc21.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "voc21 disabled, check uart wiring (tx=%d rx=%d)",
                 board_s3_spi_tft::kVocUartTx, board_s3_spi_tft::kVocUartRx);
    }
#endif

    // ==================== 6. PWM 风扇 ====================
    static FanControl fan;
    esp_err_t fan_err = fan.Init(board_s3_spi_tft::kFanPwmGpio,
                                 board_s3_spi_tft::kFanPwmFreq,
                                 board_s3_spi_tft::kFanPwmRes);
    if (fan_err != ESP_OK) {
        ESP_LOGW(TAG, "fan control disabled: %s", esp_err_to_name(fan_err));
    }
    c.fan = &fan;  // 回填给 wifi_portal（网页风扇开关），无论 Init 成败都可安全调用

    // ==================== 7. SPI TFT + 触摸 ====================
    static SpitftTouchDisplay display;
    static DisplayContext dctx;
    dctx.config   = c.config;
    dctx.registry = c.registry;
    dctx.hw       = &hw;

    // 从板型引脚定义组装 SPI TFT 引脚结构
    SpitftPins tft_pins = {};
    tft_pins.spi_host = SPI2_HOST;
    tft_pins.lcd_cs   = board_s3_spi_tft::kLcdCs;
    tft_pins.lcd_dc   = board_s3_spi_tft::kLcdDc;
    tft_pins.lcd_rst  = board_s3_spi_tft::kLcdRst;
    tft_pins.lcd_bl   = board_s3_spi_tft::kLcdBl;
    tft_pins.touch_cs = board_s3_spi_tft::kTouchCs;
    tft_pins.touch_spi_host = board_s3_spi_tft::kTouchHost;

    display.Configure(tft_pins, &fan);
    esp_err_t disp_err = display.Start(dctx);
    if (disp_err != ESP_OK) {
        ESP_LOGW(TAG, "display disabled: %s", esp_err_to_name(disp_err));
    } else {
        c.display_present = true;
    }

    // ==================== 8. 自动化规则引擎 ====================
    static Automation automation;
    automation.Init(c.config, c.registry, &fan);

    ESP_LOGI(TAG, "board S3 assembled: sensors=%d fan=%s display=%s",
             c.registry->Count(),
             fan_err == ESP_OK ? "on" : "off",
             disp_err == ESP_OK ? "on" : "off");
}

} // namespace esp32node
