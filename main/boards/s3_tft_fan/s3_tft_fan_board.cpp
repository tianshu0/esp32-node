// 项目装配：ESP32-S3 N16R8 + SPI TFT (ILI9341) + XPT2046 触摸
//           + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
//
// 构造函数：创建 FanDisplay（面板/LVGL/触摸在构造内初始化）
// Assemble()：建 SPI/I2C 总线 -> 实例化传感器/风扇/自动化
#include "s3_tft_fan_board.hpp"
#include "fan_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"
#include "i2c_bus/i2c_bus.hpp"
#include "hardware_context/hardware_context.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "sensors/sensor_device.hpp"
#include "sensors/aht20_sensor.hpp"
#include "sensors/bmp280_sensor.hpp"

#include "fan_control/fan_control.hpp"
#include "automation/automation.hpp"
#include "display/no_display.hpp"

namespace esp32node {

static const char* TAG = "board-fan";

// SPI2_HOST 总线：ILI9341 LCD + XPT2046 触摸共享
static constexpr int kSpiMaxTransferBytes = 320 * 2 * 20;

S3TftFanBoard::S3TftFanBoard(NodeContext& ctx)
    : Board(ctx)
{
    // 显示对象延迟到 Assemble 创建（需要 SPI 总线先初始化）
    display_ = nullptr;
}

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
    touch_buscfg.max_transfer_sz = 32;
    ESP_ERROR_CHECK(spi_bus_initialize(pin::kTouchHost,
                                       &touch_buscfg, SPI_DMA_DISABLED));
    ESP_LOGI(TAG, "SPI3(touch) initialized: SCK=%d MOSI=%d MISO=%d CS=%d",
             pin::kTouchSck, pin::kTouchMosi, pin::kTouchMiso, pin::kTouchCs);

    // ==================== 2. I2C 总线 ====================
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
    c.fan = &fan;

    // ==================== 6. SPI TFT + 触摸 ====================
    FanDisplayPins tft_pins = {};
    tft_pins.spi_host = SPI2_HOST;
    tft_pins.lcd_cs   = pin::kLcdCs;
    tft_pins.lcd_dc   = pin::kLcdDc;
    tft_pins.lcd_rst  = pin::kLcdRst;
    tft_pins.lcd_bl   = pin::kLcdBl;
    tft_pins.touch_cs = pin::kTouchCs;
    tft_pins.touch_spi_host = pin::kTouchHost;

    FanDisplayConfig tft_cfg = {};
    tft_cfg.lcd_pclk_mhz = 20;
    tft_cfg.rotation = ScreenRotation::Portrait;
    tft_cfg.screen_w = 240;
    tft_cfg.screen_h = 320;
    tft_cfg.double_buffer = true;
    tft_cfg.buffer_rows = 20;

    // 风扇回调：lambda 捕获 fan 指针，实现依赖反转
    auto fan_cb = [&fan](bool on) {
        if (on) {
            fan.SetPower(100, true);
        } else {
            fan.SetPower(0, true);
        }
    };

    static FanDisplay display(tft_pins, tft_cfg, fan_cb);
    if (display.width() > 0) {
        display_ = &display;
        c.display_present = true;
        ESP_LOGI(TAG, "fan display created");
    } else {
        ESP_LOGW(TAG, "fan display init failed");
        display_ = nullptr;
    }

    if (!display_) {
        static NoDisplay no_display;
        display_ = &no_display;
        ESP_LOGW(TAG, "using NoDisplay");
    }

    // ==================== 7. 自动化规则引擎 ====================
    static Automation automation;
    automation.Init(c.config, c.registry, &fan);

    ESP_LOGI(TAG, "project s3_tft_fan assembled: sensors=%d fan=%s display=%s",
             c.registry->Count(),
             fan_err == ESP_OK ? "on" : "off",
             c.display_present ? "on" : "off");
}

Board& GetBoard(NodeContext& ctx)
{
    static S3TftFanBoard board(ctx);
    return board;
}

} // namespace esp32node
