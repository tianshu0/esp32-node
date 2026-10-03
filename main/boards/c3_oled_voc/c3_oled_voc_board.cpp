// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
//
// 21VOC 输出 TVOC/CH2O(甲醛)/eCO2/温度/湿度。
#include "c3_oled_voc_board.hpp"
#include "voc_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor/sensor_reading.hpp"

#include "voc21.hpp"

#include "link/ble_link.hpp"
#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>

namespace esp32node {

static const char* TAG = "board-voc";

static Voc21* s_voc21 = nullptr;
static bool s_voc21_ok = false;

C3OledVocBoard::C3OledVocBoard(NodeContext& ctx)
    : Board(ctx)
{
    display_ = nullptr;  // 需要 I2C 总线，延迟到 Assemble 创建
}

bool C3OledVocBoard::I2cProbe(uint8_t addr)
{
    return i2c_bus_ != nullptr &&
           i2c_master_probe(i2c_bus_, addr, kI2cTimeoutMs) == ESP_OK;
}

void C3OledVocBoard::Assemble()
{
    NodeContext& c = ctx_;
    namespace pin = project_c3_oled_voc;

    // ---- I2C 总线：OLED 专用（每设备 400kHz）----
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(c.config->I2cSda());
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(c.config->I2cScl());
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus_));

    // ---- UART1 总线：21VOC 空气质量模块独占（8N1，无流控）----
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 9600;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(pin::kVocUartPort, 1024, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(pin::kVocUartPort, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(pin::kVocUartPort, pin::kVocUartTx, pin::kVocUartRx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // ---- 传感器：21VOC 组件直接绑定 UART 端口 ----
    static Voc21 voc21(pin::kVocUartPort);
    s_voc21 = &voc21;
    s_voc21_ok = (voc21.Init() == ESP_OK);
    if (!s_voc21_ok) {
        ESP_LOGW(TAG, "voc21 disabled, check uart wiring (tx=%d rx=%d)",
                 pin::kVocUartTx, pin::kVocUartRx);
    }

    // ---- 能力清单 JSON ----
    if (s_voc21_ok) {
        std::snprintf(types_json_, sizeof(types_json_), "[\"air_quality\"]");
        std::snprintf(detail_json_, sizeof(detail_json_),
                      "[{\"type\":\"air_quality\",\"model\":\"21VOC\","
                      "\"format\":{\"tvoc\":\"int\",\"ch2o\":\"int\",\"eco2\":\"int\","
                      "\"temp\":\"float\",\"humidity\":\"float\","
                      "\"unit\":\"ug/ug/ppm/C/%%\"}}]");
    }

    // ---- 链路：BLE 外设 ----
#if CONFIG_BT_ENABLED
    static BleLink link(*c.config, *this);
    link_ = &link;
#endif

    // ---- 显示屏 ----
    uint8_t addr = 0;
    if (I2cProbe(0x3C)) {
        addr = 0x3C;
    } else if (I2cProbe(0x3D)) {
        addr = 0x3D;
    }

    if (addr != 0) {
        static VocDisplay display(i2c_bus_, addr, 128, 64, true, true);
        if (display.width() > 0) {
            display_ = &display;
            c.display_present = true;
            display.BuildUi(c.config->NodeId().c_str());
            ESP_LOGI(TAG, "voc display created at 0x%02X", addr);
        } else {
            ESP_LOGW(TAG, "voc display init failed");
            display_ = nullptr;
        }
    }

    if (!display_) {
        static NoDisplay no_display;
        display_ = &no_display;
        ESP_LOGW(TAG, "oled not found at 0x3C/0x3D, using NoDisplay");
    }
}

int C3OledVocBoard::ReadSensors(SensorReading* out, int max)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int n = 0;

    if (s_voc21_ok && s_voc21 && n < max) {
        uint16_t tvoc = 0, ch2o = 0, eco2 = 0;
        float t = 0.0f, h = 0.0f;
        if (s_voc21->Read(&tvoc, &ch2o, &eco2, &t, &h)) {
            std::strncpy(out[n].type, "air_quality", sizeof(out[n].type) - 1);
            out[n].type[sizeof(out[n].type) - 1] = '\0';
            out[n].ts_ms = esp_timer_get_time() / 1000;
            std::snprintf(out[n].values_json, sizeof(out[n].values_json),
                          "{\"tvoc\":%u,\"ch2o\":%u,\"eco2\":%u,\"temp\":%.1f,\"humidity\":%.1f}",
                          static_cast<unsigned>(tvoc), static_cast<unsigned>(ch2o),
                          static_cast<unsigned>(eco2),
                          static_cast<double>(t), static_cast<double>(h));
            ++n;
        }
    }
    return n;
}

Board& GetBoard(NodeContext& ctx)
{
    static C3OledVocBoard board(ctx);
    return board;
}

} // namespace esp32node
