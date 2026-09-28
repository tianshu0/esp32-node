// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
//
// 本文件是全项目唯一知道具体硬件组合的文件：
//   - 总线/传感器/屏的具体类名只出现在这里，main.cpp 与核心组件保持硬件无关
//   - 单个传感器初始化失败只告警跳过（I2C 地址无应答），节点带可用部件继续运行
//   - 屏未接同样只告警，不影响 BLE 广播与数据上报
#include "C3OledThpBoard.hpp"
#include "config.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "hardware_context/HardwareContext.hpp"

#include "sensors/SensorDevice.hpp"
#include "sensors/Sht3xSensor.hpp"
#include "sensors/Bmp180Sensor.hpp"

#include "display/DisplayContext.hpp"
#include "display/Ssd1315Display.hpp"
#include "display/DashboardScreen.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "board-thp";

void C3OledThpBoard::Assemble()
{
    NodeContext& c = ctx_;
    // ---- I2C 总线：SHT3X / BMP180 / OLED 共用（400kHz，互斥由 I2cBus 保证）----
    static I2cBus i2c;
    ESP_ERROR_CHECK(i2c.Init(c.config->I2cSda(), c.config->I2cScl()));

    // 必须 static：显示驱动 Start() 会把 DisplayContext（含 hw 指针）存入成员，
    // 刷新任务持续解引用；栈对象在函数返回后即悬空。
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

    // ---- 显示屏：驱动（什么屏）与内容模板（显示什么）在装配处组合 ----
    static DashboardScreen screen;
    static Ssd1315Display display;
    static DisplayContext dctx;  // 理由同上：指针被驱动长期持有
    dctx.config = c.config;
    dctx.registry = c.registry;
    dctx.ble = c.ble;
    dctx.hw = &hw;
    dctx.screen = &screen;

    esp_err_t err = display.Start(dctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display disabled: %s", esp_err_to_name(err));
    } else {
        c.display_present = true;
    }
}

Board& GetBoard(NodeContext& ctx)
{
    static C3OledThpBoard board(ctx);
    return board;
}

} // namespace esp32node
