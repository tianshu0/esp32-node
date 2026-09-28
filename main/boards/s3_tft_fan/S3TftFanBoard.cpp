// 项目装配：ESP32-S3 N16R8 + SPI TFT (ILI9341) + XPT2046 触摸
//           + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
//
// 硬件连接见同目录 config.h；本文件负责：
//   1. SPI2_HOST 总线（LCD + 触摸共享）
//   2. I2C 总线（AHT20 0x38 / BMP280 0x76|0x77 共用）
//   3. LEDC PWM 风扇控制
//   4. SPI TFT 显示驱动（ILI9341 + XPT2046 触摸 + LVGL 多页 UI）
//   5. 自动化规则引擎（阈值触发 + 定时时长 + 迟滞保护）
#include "S3TftFanBoard.hpp"
#include "config.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "hardware_context/HardwareContext.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "sensors/SensorDevice.hpp"
#include "sensors/Aht20Sensor.hpp"
#include "sensors/Bmp280Sensor.hpp"

#include "fan_control/FanControl.hpp"
#include "automation/Automation.hpp"
#include "display/SpitftTouchDisplay.hpp"

namespace esp32node {

static const char* TAG = "board-fan";

// SPI2_HOST 总线：ILI9341 LCD + XPT2046 触摸共享
// max_transfer_sz 按最大分区缓冲：横屏 320*20*2=12800B（竖屏 240*20*2=9600B）
static constexpr int kSpiMaxTransferBytes = 320 * 2 * 20;

void S3TftFanBoard::Assemble()
{
    NodeContext& c = ctx_;
    namespace pin = project_s3_tft_fan;

    // ==================== 1. SPI2_HOST 总线 ====================
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = pin::kSpiMosi;
    buscfg.miso_io_num = pin::kSpiMiso;
    buscfg.sclk_io_num = pin::kSpiSclk;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = kSpiMaxTransferBytes;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI2(LCD) initialized: MOSI=%d MISO=%d SCLK=%d",
             pin::kSpiMosi, pin::kSpiMiso, pin::kSpiSclk);

    // ==================== 1b. SPI3_HOST 触摸专用总线（无 DMA）====================
    spi_bus_config_t touch_buscfg = {};
    touch_buscfg.mosi_io_num = pin::kTouchMosi;
    touch_buscfg.miso_io_num = pin::kTouchMiso;
    touch_buscfg.sclk_io_num = pin::kTouchSck;
    touch_buscfg.quadwp_io_num = -1;
    touch_buscfg.quadhd_io_num = -1;
    touch_buscfg.max_transfer_sz = 32;  // XPT2046 单次仅 3 字节
    ESP_ERROR_CHECK(spi_bus_initialize(pin::kTouchHost,
                                       &touch_buscfg, SPI_DMA_DISABLED));
    ESP_LOGI(TAG, "SPI3(touch) initialized: SCK=%d MOSI=%d MISO=%d CS=%d",
             pin::kTouchSck, pin::kTouchMosi, pin::kTouchMiso, pin::kTouchCs);

    // ==================== 2. I2C 总线 ====================
    // 引脚以项目 config.h 为唯一事实源（不使用 AppConfig 的运行时默认值 4/5，
    // 那是 C3 板型遗留；S3 二合一模块按 config.h 接在 GPIO8/9）。
    static I2cBus i2c;
    ESP_ERROR_CHECK(i2c.Init(pin::kI2cSda, pin::kI2cScl));

    // ==================== 3. 硬件上下文 ====================
    static HardwareContext hw;
    hw.i2c = &i2c;

    // ==================== 4. 传感器 ====================
    static Aht20Sensor aht20;
    if (aht20.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "aht20 disabled, check wiring (addr 0x38)");
    }

    static Bmp280Sensor bmp280;
    if (bmp280.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "bmp280 disabled, check wiring (addr 0x76/0x77)");
    }

    // ==================== 5. PWM 风扇 ====================
    static FanControl fan;
    esp_err_t fan_err = fan.Init(pin::kFanPwmGpio,
                                 pin::kFanPwmFreq,
                                 pin::kFanPwmRes);
    if (fan_err != ESP_OK) {
        ESP_LOGW(TAG, "fan control disabled: %s", esp_err_to_name(fan_err));
    }
    c.fan = &fan;  // 回填给 wifi_portal（网页风扇开关），无论 Init 成败都可安全调用

    // ==================== 6. SPI TFT + 触摸 ====================
    static SpitftTouchDisplay display;
    static DisplayContext dctx;
    dctx.config   = c.config;
    dctx.registry = c.registry;
    dctx.hw       = &hw;

    // 从项目引脚定义组装 SPI TFT 引脚结构
    SpitftPins tft_pins = {};
    tft_pins.spi_host = SPI2_HOST;
    tft_pins.lcd_cs   = pin::kLcdCs;
    tft_pins.lcd_dc   = pin::kLcdDc;
    tft_pins.lcd_rst  = pin::kLcdRst;
    tft_pins.lcd_bl   = pin::kLcdBl;
    tft_pins.touch_cs = pin::kTouchCs;
    tft_pins.touch_spi_host = pin::kTouchHost;

    display.Configure(tft_pins, &fan);
    esp_err_t disp_err = display.Start(dctx);
    if (disp_err != ESP_OK) {
        ESP_LOGW(TAG, "display disabled: %s", esp_err_to_name(disp_err));
    } else {
        c.display_present = true;
    }

    // ==================== 7. 自动化规则引擎 ====================
    static Automation automation;
    automation.Init(c.config, c.registry, &fan);

    ESP_LOGI(TAG, "project s3_tft_fan assembled: sensors=%d fan=%s display=%s",
             c.registry->Count(),
             fan_err == ESP_OK ? "on" : "off",
             disp_err == ESP_OK ? "on" : "off");
}

Board& GetBoard(NodeContext& ctx)
{
    static S3TftFanBoard board(ctx);
    return board;
}

} // namespace esp32node
