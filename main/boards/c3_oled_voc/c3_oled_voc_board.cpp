// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
//
// 21VOC 输出 TVOC/CH2O(甲醛)/eCO2/温度/湿度。
#include "c3_oled_voc_board.hpp"
#include "voc_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"
#include "i2c_bus/i2c_bus.hpp"
#include "uart_bus/uart_bus.hpp"
#include "hardware_context/hardware_context.hpp"

#include "sensors/sensor_device.hpp"
#include "sensors/voc21_sensor.hpp"

#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "board-voc";

C3OledVocBoard::C3OledVocBoard(NodeContext& ctx)
    : Board(ctx)
{
    display_ = nullptr;
}

void C3OledVocBoard::Assemble()
{
    NodeContext& c = ctx_;
    // ---- I2C 总线：OLED 专用（400kHz）----
    static I2cBus i2c;
    ESP_ERROR_CHECK(i2c.Init(c.config->I2cSda(), c.config->I2cScl()));

    // ---- UART1 总线：21VOC 空气质量模块独占（点对点，无仲裁）----
    static UartBus uart;
    ESP_ERROR_CHECK(uart.Init(project_c3_oled_voc::kVocUartPort,
                              project_c3_oled_voc::kVocUartTx,
                              project_c3_oled_voc::kVocUartRx));

    static HardwareContext hw;
    hw.i2c = &i2c;
    hw.uart = &uart;

    // ---- 传感器：驱动在 Start() 内自登记到 registry ----
    static Voc21Sensor voc21;
    if (voc21.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "voc21 disabled, check uart wiring (tx=%d rx=%d)",
                 project_c3_oled_voc::kVocUartTx, project_c3_oled_voc::kVocUartRx);
    }

    // ---- 显示屏：探测面板地址，成功则创建 VocDisplay，失败则 NoDisplay ----
    uint8_t addr = 0;
    if (i2c.Probe(0x3C)) {
        addr = 0x3C;
    } else if (i2c.Probe(0x3D)) {
        addr = 0x3D;
    }

    if (addr != 0) {
        static VocDisplay display(&i2c, addr, 128, 64, true, true);
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

Board& GetBoard(NodeContext& ctx)
{
    static C3OledVocBoard board(ctx);
    return board;
}

} // namespace esp32node
