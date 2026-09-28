// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
//
// 21VOC 输出 TVOC/CH2O(甲醛)/eCO2/温度/湿度。
// 本文件是全项目唯一知道具体硬件组合的文件：
//   - 总线/传感器/屏的具体类名只出现在这里，main.cpp 与核心组件保持硬件无关
//   - 传感器初始化失败只告警跳过，节点带可用部件继续运行
//   - 屏未接同样只告警，不影响 BLE 广播与数据上报
#include "C3OledVocBoard.hpp"
#include "config.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "uart_bus/UartBus.hpp"
#include "hardware_context/HardwareContext.hpp"

#include "sensors/SensorDevice.hpp"
#include "sensors/Voc21Sensor.hpp"

#include "display/DisplayContext.hpp"
#include "display/Ssd1315Display.hpp"
#include "display/DashboardScreen.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "board-voc";

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

    // 必须 static：显示驱动 Start() 会把 DisplayContext（含 hw 指针）存入成员，
    // 刷新任务持续解引用；栈对象在函数返回后即悬空。
    static HardwareContext hw;
    hw.i2c = &i2c;
    hw.uart = &uart;

    // ---- 传感器：驱动在 Start() 内自登记到 registry ----
    static Voc21Sensor voc21;
    if (voc21.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "voc21 disabled, check uart wiring (tx=%d rx=%d)",
                 project_c3_oled_voc::kVocUartTx, project_c3_oled_voc::kVocUartRx);
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
    static C3OledVocBoard board(ctx);
    return board;
}

} // namespace esp32node
