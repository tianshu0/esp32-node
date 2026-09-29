// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
//
// 构造函数：探测 SSD1315 面板（0x3C/0x3D），成功则创建 ThpDisplay，失败则 NoDisplay
// Assemble()：建 I2C 总线 -> 实例化传感器（面板已在构造时创建）
#include "c3_oled_thp_board.hpp"
#include "thp_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"
#include "i2c_bus/i2c_bus.hpp"
#include "hardware_context/hardware_context.hpp"

#include "sensors/sensor_device.hpp"
#include "sensors/sht3x_sensor.hpp"
#include "sensors/bmp180_sensor.hpp"

#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "board-thp";

C3OledThpBoard::C3OledThpBoard(NodeContext& ctx)
    : Board(ctx)
{
    // 探测 SSD1315 面板（0x3C 为主，部分板子 0x3D）
    // 注意：I2C 总线尚未初始化，探测在 Assemble 后由 ThpDisplay 构造函数完成
    // 此处仅根据 Kconfig 选择创建对象，实际初始化在构造内
    // 简化：直接尝试创建，失败由 ThpDisplay 内部处理（对象仍有效，只是无屏）
    display_ = nullptr;  // 延迟到 Assemble 创建，因为需要 I2C 总线
}

void C3OledThpBoard::Assemble()
{
    NodeContext& c = ctx_;
    // ---- I2C 总线：SHT3X / BMP180 / OLED 共用（400kHz，互斥由 I2cBus 保证）----
    static I2cBus i2c;
    ESP_ERROR_CHECK(i2c.Init(c.config->I2cSda(), c.config->I2cScl()));

    // 必须 static：传感器驱动 Start() 内只取句柄不存指针，但 Board 成员 display_ 需要长期持有
    static HardwareContext hw;
    hw.i2c = &i2c;

    // ---- 传感器：驱动在 Start() 内自登记到 registry ----
    static Sht3xSensor sht3x;
    if (sht3x.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "sht3x disabled, check wiring (addr 0x44/0x45)");
    }

    static Bmp180Sensor bmp180;
    if (bmp180.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "bmp180 disabled, check wiring (addr 0x77)");
    }

    // ---- 显示屏：探测面板地址，成功则创建 ThpDisplay，失败则 NoDisplay ----
    uint8_t addr = 0;
    if (i2c.Probe(0x3C)) {
        addr = 0x3C;
    } else if (i2c.Probe(0x3D)) {
        addr = 0x3D;
    }

    if (addr != 0) {
        static ThpDisplay display(&i2c, addr, 128, 64, true, true);
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

Board& GetBoard(NodeContext& ctx)
{
    static C3OledThpBoard board(ctx);
    return board;
}

} // namespace esp32node
