// 项目装配：ESP32-S3 N16R8 + SPI TFT (ILI9341) + XPT2046 触摸
//           + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
//
// 构造函数：创建 FanDisplay（面板/LVGL/触摸在构造内初始化）
// Assemble()：建 SPI/I2C 总线 -> 实例化传感器/风扇/自动化
#include "s3_tft_fan_board.hpp"
#include "fan_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor/sensor_reading.hpp"

#include "aht20.hpp"
#include "bmp280.hpp"

#include "fan_control/fan_control.hpp"
#include "automation/automation.hpp"
#include "link/wifi_link.hpp"
#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include <cstdio>
#include <cstring>

namespace esp32node {

static const char* TAG = "board-fan";

// SPI2_HOST 总线：ILI9341 LCD + XPT2046 触摸共享
static constexpr int kSpiMaxTransferBytes = 320 * 2 * 20;

// 本板传感器实例
static Aht20*  s_aht20  = nullptr;
static Bmp280* s_bmp280 = nullptr;
static bool s_aht20_ok  = false;
static bool s_bmp280_ok = false;

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
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = I2C_NUM_0;
    i2c_cfg.sda_io_num = static_cast<gpio_num_t>(pin::kI2cSda);
    i2c_cfg.scl_io_num = static_cast<gpio_num_t>(pin::kI2cScl);
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus_));

    // ==================== 3. 传感器（独立组件，直接挂载 I2C 总线）====================
    static Aht20 aht20(i2c_bus_, Aht20::kAddr);
    s_aht20 = &aht20;
    s_aht20_ok = (aht20.Init() == ESP_OK);
    if (!s_aht20_ok) {
        ESP_LOGW(TAG, "aht20 disabled, check wiring (addr 0x38)");
    }

    static Bmp280 bmp280(i2c_bus_);
    s_bmp280 = &bmp280;
    s_bmp280_ok = (bmp280.Init(c.config->SeaLevelHpa()) == ESP_OK);
    if (!s_bmp280_ok) {
        ESP_LOGW(TAG, "bmp280 disabled, check wiring (addr 0x76/0x77)");
    }

    // ---- 能力清单 JSON ----
    int toff = std::snprintf(types_json_, sizeof(types_json_), "[");
    int doff = std::snprintf(detail_json_, sizeof(detail_json_), "[");
    bool first = true;
    if (s_aht20_ok) {
        toff += std::snprintf(types_json_ + toff, sizeof(types_json_) - toff,
                              "%s\"temp_hum\"", first ? "" : ",");
        doff += std::snprintf(detail_json_ + doff, sizeof(detail_json_) - doff,
                              "%s{\"type\":\"temp_hum\",\"model\":\"AHT20\","
                              "\"format\":{\"temp\":\"float\",\"humidity\":\"float\","
                              "\"unit\":\"C/%%\"}}", first ? "" : ",");
        first = false;
    }
    if (s_bmp280_ok) {
        toff += std::snprintf(types_json_ + toff, sizeof(types_json_) - toff,
                              "%s\"pressure\"", first ? "" : ",");
        doff += std::snprintf(detail_json_ + doff, sizeof(detail_json_) - doff,
                              "%s{\"type\":\"pressure\",\"model\":\"BMP280\","
                              "\"format\":{\"pressure\":\"float\",\"temp\":\"float\","
                              "\"altitude\":\"float\",\"unit\":\"hPa/C/m\"}}",
                              first ? "" : ",");
        first = false;
    }
    std::snprintf(types_json_ + toff, sizeof(types_json_) - toff, "]");
    std::snprintf(detail_json_ + doff, sizeof(detail_json_) - doff, "]");

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
    automation.Init(c.config, this, &fan);

    // ==================== 7. 链路：WiFi（SoftAP 配网 + STA 上报）====================
    // 风扇控制以回调注入（依赖反转）：wifi 组件不依赖 FanControl，只有本板知道风扇。
    // 风扇初始化失败时不提供 hooks，网页自动隐藏风扇开关。
    FanHooks fan_hooks;
    if (fan_err == ESP_OK) {
        fan_hooks.is_running = [&fan]() { return fan.IsRunning(); };
        fan_hooks.power      = [&fan]() { return fan.CurrentPower(); };
        fan_hooks.set_power  = [&fan](int pct) { fan.SetPower(pct, true); };
    }
    static WifiLink link(*c.config, *this, fan_hooks);
    link_ = &link;

    ESP_LOGI(TAG, "project s3_tft_fan assembled: sensors=%d fan=%s display=%s",
             (s_aht20_ok ? 1 : 0) + (s_bmp280_ok ? 1 : 0),
             fan_err == ESP_OK ? "on" : "off",
             c.display_present ? "on" : "off");
}

int S3TftFanBoard::ReadSensors(SensorReading* out, int max)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int n = 0;

    if (s_aht20_ok && s_aht20 && n < max) {
        float t = 0.0f, h = 0.0f;
        if (s_aht20->Read(&t, &h)) {
            std::strncpy(out[n].type, "temp_hum", sizeof(out[n].type) - 1);
            out[n].type[sizeof(out[n].type) - 1] = '\0';
            out[n].ts_ms = esp_timer_get_time() / 1000;
            std::snprintf(out[n].values_json, sizeof(out[n].values_json),
                          "{\"temp\":%.2f,\"humidity\":%.2f}",
                          static_cast<double>(t), static_cast<double>(h));
            ++n;
        }
    }

    if (s_bmp280_ok && s_bmp280 && n < max) {
        float t = 0.0f, p = 0.0f, alt = 0.0f;
        if (s_bmp280->Read(&t, &p, &alt)) {
            std::strncpy(out[n].type, "pressure", sizeof(out[n].type) - 1);
            out[n].type[sizeof(out[n].type) - 1] = '\0';
            out[n].ts_ms = esp_timer_get_time() / 1000;
            std::snprintf(out[n].values_json, sizeof(out[n].values_json),
                          "{\"pressure\":%.2f,\"temp\":%.2f,\"altitude\":%.1f}",
                          static_cast<double>(p), static_cast<double>(t),
                          static_cast<double>(alt));
            ++n;
        }
    }
    return n;
}

Board& GetBoard(NodeContext& ctx)
{
    static S3TftFanBoard board(ctx);
    return board;
}

} // namespace esp32node
