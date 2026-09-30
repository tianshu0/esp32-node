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

#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "sensors/sensor_device.hpp"
#include "sensors/aht20_sensor.hpp"
#include "sensors/bmp280_sensor.hpp"

#include "fan_control/fan_control.hpp"
#include "automation/automation.hpp"
#include "link/wifi_link.hpp"
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

i2c_master_dev_handle_t S3TftFanBoard::I2cDevice(uint8_t addr)
{
    for (size_t i = 0; i < i2c_dev_count_; ++i) {
        if (i2c_devs_[i].addr == addr) {
            return i2c_devs_[i].dev;
        }
    }
    if (i2c_bus_ == nullptr || i2c_dev_count_ >= kMaxI2cDevs) {
        return nullptr;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = kI2cClkHz;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device 0x%02X failed", addr);
        return nullptr;
    }
    i2c_devs_[i2c_dev_count_++] = {addr, dev};
    return dev;
}

esp_err_t S3TftFanBoard::I2cWrite(uint8_t addr, const uint8_t* data, size_t len)
{
    i2c_master_dev_handle_t dev = I2cDevice(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev, data, len, kI2cTimeoutMs);
}

esp_err_t S3TftFanBoard::I2cRead(uint8_t addr, uint8_t* buf, size_t len)
{
    i2c_master_dev_handle_t dev = I2cDevice(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_receive(dev, buf, len, kI2cTimeoutMs);
}

esp_err_t S3TftFanBoard::I2cWriteRead(uint8_t addr, const uint8_t* w, size_t wlen,
                                      uint8_t* r, size_t rlen)
{
    i2c_master_dev_handle_t dev = I2cDevice(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, w, wlen, r, rlen, kI2cTimeoutMs);
}

bool S3TftFanBoard::I2cProbe(uint8_t addr)
{
    return i2c_bus_ != nullptr &&
           i2c_master_probe(i2c_bus_, addr, kI2cTimeoutMs) == ESP_OK;
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
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = I2C_NUM_0;
    i2c_cfg.sda_io_num = static_cast<gpio_num_t>(pin::kI2cSda);
    i2c_cfg.scl_io_num = static_cast<gpio_num_t>(pin::kI2cScl);
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus_));

    // ==================== 3. 传感器（经本板的 I2C 原语访问总线）====================
    static Aht20Sensor aht20;
    if (aht20.Start(*this, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "aht20 disabled, check wiring (addr 0x38)");
    }

    static Bmp280Sensor bmp280;
    if (bmp280.Start(*this, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "bmp280 disabled, check wiring (addr 0x76/0x77)");
    }

    // ==================== 4. PWM 风扇 ====================
    static FanControl fan;
    esp_err_t fan_err = fan.Init(pin::kFanPwmGpio,
                                 pin::kFanPwmFreq,
                                 pin::kFanPwmRes);
    if (fan_err != ESP_OK) {
        ESP_LOGW(TAG, "fan control disabled: %s", esp_err_to_name(fan_err));
    }

    // ==================== 5. SPI TFT + 触摸 ====================
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

    // ==================== 6. 自动化规则引擎 ====================
    static Automation automation;
    automation.Init(c.config, c.registry, &fan);

    // ==================== 7. 链路：WiFi（SoftAP 配网 + STA 上报）====================
    // 风扇控制以回调注入（依赖反转）：wifi 组件不依赖 FanControl，只有本板知道风扇。
    // 风扇初始化失败时不提供 hooks，网页自动隐藏风扇开关。
    FanHooks fan_hooks;
    if (fan_err == ESP_OK) {
        fan_hooks.is_running = [&fan]() { return fan.IsRunning(); };
        fan_hooks.power      = [&fan]() { return fan.CurrentPower(); };
        fan_hooks.set_power  = [&fan](int pct) { fan.SetPower(pct, true); };
    }
    static WifiLink link(*c.config, *c.registry, fan_hooks);
    link_ = &link;

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
