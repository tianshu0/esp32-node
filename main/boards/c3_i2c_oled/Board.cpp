// 板型装配实现：ESP32-C3 + I2C 传感器组 + SSD1315 OLED
//
// 这里是「编译期选择」落地的地方：
//   - 总线/传感器/屏的具体类名只出现在本文件，main.cpp 与核心组件保持硬件无关
//   - 未在 menuconfig 勾选的驱动不参与编译（#if 包裹），不占 flash/RAM
//   - 单个传感器初始化失败只告警跳过（I2C 地址无应答），节点带可用部件继续运行
//   - 屏未接同样只告警，不影响 BLE 广播与数据上报
#include "Board.hpp"
#include "c3_i2c_oled/Pins.hpp"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "ble_peripheral/BlePeripheral.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "uart_bus/UartBus.hpp"
#include "hardware_context/HardwareContext.hpp"

#include "esp_err.h"
#include "esp_log.h"

#if CONFIG_NODE_SENSOR_SHT3X || CONFIG_NODE_SENSOR_BMP180 || CONFIG_NODE_SENSOR_VOC21
#include "sensors/SensorDevice.hpp"
#endif
#if CONFIG_NODE_SENSOR_SHT3X
#include "sensors/Sht3xSensor.hpp"
#endif
#if CONFIG_NODE_SENSOR_BMP180
#include "sensors/Bmp180Sensor.hpp"
#endif
#if CONFIG_NODE_SENSOR_VOC21
#include "sensors/Voc21Sensor.hpp"
#endif

#if CONFIG_NODE_DISPLAY_SSD1315
#include "display_service/DisplayContext.hpp"
#include "display_service/Ssd1315Display.hpp"
#include "display_service/DashboardScreen.hpp"
#endif

#if CONFIG_NODE_DISPLAY_SSD1306_128X32
#include "display_service/DisplayContext.hpp"
#include "display_service/Ssd1306Display.hpp"
#include "display_service/CompactDashboardScreen.hpp"
#endif

namespace esp32node {

static const char* TAG = "board";

void BoardAssemble(NodeContext& c)
{
    // ---- I2C 总线：本板所有 I2C 传感器与 OLED 共用（400kHz，互斥由 I2cBus 保证）----
    static I2cBus i2c;
    ESP_ERROR_CHECK(i2c.Init(c.config->I2cSda(), c.config->I2cScl()));

    // ---- UART 总线：21VOC 空气质量模块独占 UART1（点对点，无仲裁）----
#if CONFIG_NODE_SENSOR_VOC21
    static UartBus uart;
    ESP_ERROR_CHECK(uart.Init(board_c3_i2c_oled::kVocUartPort,
                              board_c3_i2c_oled::kVocUartTx,
                              board_c3_i2c_oled::kVocUartRx));
#endif

    // 注意必须 static：显示驱动 Start() 会把 DisplayContext（含 hw 指针）存入成员，
    // 刷新任务在其后持续解引用；栈对象在函数返回后即悬空（运行期表现为
    // I2cBus::Unlock() 对空指针取成员的 Load access fault）。
    static HardwareContext hw;
    hw.i2c = &i2c;
#if CONFIG_NODE_SENSOR_VOC21
    hw.uart = &uart;
#endif

    // ---- 传感器：勾选了哪些就实例化哪些；驱动在 Start() 内自登记到 registry ----
#if CONFIG_NODE_SENSOR_SHT3X
    static Sht3xSensor sht3x;
    if (sht3x.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "sht3x disabled, check wiring (addr 0x44/0x45)");
    }
#endif

#if CONFIG_NODE_SENSOR_BMP180
    static Bmp180Sensor bmp180;
    if (bmp180.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "bmp180 disabled, check wiring (addr 0x77)");
    }
#endif

#if CONFIG_NODE_SENSOR_VOC21
    static Voc21Sensor voc21;
    if (voc21.Start(hw, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "voc21 disabled, check uart wiring (tx=%d rx=%d)",
                 board_c3_i2c_oled::kVocUartTx, board_c3_i2c_oled::kVocUartRx);
    }
#endif

    // ---- 显示屏：驱动（什么屏）与内容模板（显示什么）在装配处组合 ----
#if CONFIG_NODE_DISPLAY_SSD1315
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
#endif

#if CONFIG_NODE_DISPLAY_SSD1306_128X32
    static CompactDashboardScreen screen;
    static Ssd1306Display display;
    static DisplayContext dctx;
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
#endif
}

} // namespace esp32node
