// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
//
// Assemble()：建 I2C 总线 -> 实例化传感器组件 -> 探测面板地址并创建 ThpDisplay
#include "c3_oled_thp_board.hpp"
#include "thp_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor/sensor_reading.hpp"

#include "sht3x.hpp"
#include "bmp180.hpp"

#include "link/ble_link.hpp"
#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>

namespace esp32node {

static const char* TAG = "board-thp";

// 本板传感器实例（文件域 static，生命周期覆盖运行期）
static Sht3x*  s_sht3x  = nullptr;
static Bmp180* s_bmp180 = nullptr;
static bool s_sht3x_ok  = false;
static bool s_bmp180_ok = false;

C3OledThpBoard::C3OledThpBoard(NodeContext& ctx)
    : Board(ctx)
{
    display_ = nullptr;  // 需要 I2C 总线，延迟到 Assemble 创建
}

bool C3OledThpBoard::I2cProbe(uint8_t addr)
{
    return i2c_bus_ != nullptr &&
           i2c_master_probe(i2c_bus_, addr, kI2cTimeoutMs) == ESP_OK;
}

void C3OledThpBoard::Assemble()
{
    NodeContext& c = ctx_;

    // ---- I2C 总线：SHT3X / BMP180 / OLED 共用（每设备 400kHz）----
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(c.config->I2cSda());
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(c.config->I2cScl());
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus_));

    // ---- 传感器：独立组件，直接挂载到本板 I2C 总线 ----
    static Sht3x sht3x(i2c_bus_, Sht3x::kAddrDefault);
    s_sht3x = &sht3x;
    s_sht3x_ok = (sht3x.Init() == ESP_OK);
    if (!s_sht3x_ok) {
        ESP_LOGW(TAG, "sht3x disabled, check wiring (addr 0x44/0x45)");
    }

    static Bmp180 bmp180(i2c_bus_, Bmp180::kAddr);
    s_bmp180 = &bmp180;
    s_bmp180_ok = (bmp180.Init(c.config->SeaLevelHpa()) == ESP_OK);
    if (!s_bmp180_ok) {
        ESP_LOGW(TAG, "bmp180 disabled, check wiring (addr 0x77)");
    }

    // ---- 能力清单 JSON（BLE 握手用，仅含初始化成功的传感器）----
    int toff = std::snprintf(types_json_, sizeof(types_json_), "[");
    int doff = std::snprintf(detail_json_, sizeof(detail_json_), "[");
    bool first = true;
    if (s_sht3x_ok) {
        toff += std::snprintf(types_json_ + toff, sizeof(types_json_) - toff,
                              "%s\"temp_hum\"", first ? "" : ",");
        doff += std::snprintf(detail_json_ + doff, sizeof(detail_json_) - doff,
                              "%s{\"type\":\"temp_hum\",\"model\":\"SHT3X\","
                              "\"format\":{\"temp\":\"float\",\"humidity\":\"float\","
                              "\"unit\":\"C/%%\"}}", first ? "" : ",");
        first = false;
    }
    if (s_bmp180_ok) {
        toff += std::snprintf(types_json_ + toff, sizeof(types_json_) - toff,
                              "%s\"pressure\"", first ? "" : ",");
        doff += std::snprintf(detail_json_ + doff, sizeof(detail_json_) - doff,
                              "%s{\"type\":\"pressure\",\"model\":\"BMP180\","
                              "\"format\":{\"pressure\":\"float\",\"temp\":\"float\","
                              "\"altitude\":\"float\",\"unit\":\"hPa/C/m\"}}",
                              first ? "" : ",");
        first = false;
    }
    std::snprintf(types_json_ + toff, sizeof(types_json_) - toff, "]");
    std::snprintf(detail_json_ + doff, sizeof(detail_json_) - doff, "]");

    // ---- 链路：BLE 外设（本板是 BLE 节点，经 BLE 与 hub 握手/周期上报）----
#if CONFIG_BT_ENABLED
    static BleLink link(*c.config, *this);
    link_ = &link;
#endif

    // ---- 显示屏：探测面板地址，成功则创建 ThpDisplay，失败则 NoDisplay ----
    uint8_t addr = 0;
    if (I2cProbe(0x3C)) {
        addr = 0x3C;
    } else if (I2cProbe(0x3D)) {
        addr = 0x3D;
    }

    if (addr != 0) {
        static ThpDisplay display(i2c_bus_, addr, 128, 64, true, true);
        if (display.width() > 0) {  // 构造成功（面板/LVGL 已初始化）
            display_ = &display;
            c.display_present = true;
            // 构建静态 UI（需要 node_id，从 config 取）
            display.BuildUi(c.config->NodeId().c_str());
            ESP_LOGI(TAG, "thp display created at 0x%02X", addr);
        } else {
            ESP_LOGW(TAG, "thp display init failed");
            display_ = nullptr;
        }
    }

    if (!display_) {
        static NoDisplay no_display;
        display_ = &no_display;
        ESP_LOGW(TAG, "oled not found at 0x3C/0x3D, using NoDisplay");
    }
}

// 板级解析：把各传感器原始读数转成 SensorReading（type + values_json）
int C3OledThpBoard::ReadSensors(SensorReading* out, int max)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int n = 0;

    if (s_sht3x_ok && s_sht3x && n < max) {
        float t = 0.0f, h = 0.0f;
        if (s_sht3x->Read(&t, &h)) {
            std::strncpy(out[n].type, "temp_hum", sizeof(out[n].type) - 1);
            out[n].type[sizeof(out[n].type) - 1] = '\0';
            out[n].ts_ms = esp_timer_get_time() / 1000;
            std::snprintf(out[n].values_json, sizeof(out[n].values_json),
                          "{\"temp\":%.2f,\"humidity\":%.2f}",
                          static_cast<double>(t), static_cast<double>(h));
            ++n;
        }
    }

    if (s_bmp180_ok && s_bmp180 && n < max) {
        float t = 0.0f, p = 0.0f, alt = 0.0f;
        if (s_bmp180->Read(&t, &p, &alt)) {
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
    static C3OledThpBoard board(ctx);
    return board;
}

} // namespace esp32node
