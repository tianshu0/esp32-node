// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + 21VOC 五合一空气质量模块（UART）
//
// 21VOC 输出 TVOC/CH2O(甲醛)/eCO2/温度/湿度。
#include "c3_oled_voc_board.hpp"
#include "voc_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"

#include "sensors/sensor_device.hpp"
#include "sensors/voc21_sensor.hpp"

#include "link/ble_link.hpp"
#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "board-voc";

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

int C3OledVocBoard::UartRead(uint8_t* buf, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(project_c3_oled_voc::kVocUartPort, buf, len,
                           pdMS_TO_TICKS(timeout_ms));
}

int C3OledVocBoard::UartWrite(const uint8_t* buf, size_t len)
{
    return uart_write_bytes(project_c3_oled_voc::kVocUartPort, buf, len);
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

    // ---- 传感器：驱动经本板的 UART 原语读数据，Start() 内自登记 ----
    static Voc21Sensor voc21;
    if (voc21.Start(*this, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "voc21 disabled, check uart wiring (tx=%d rx=%d)",
                 pin::kVocUartTx, pin::kVocUartRx);
    }

    // ---- 链路：BLE 外设（本板是 BLE 节点，经 BLE 与 hub 握手/周期上报）----
#if CONFIG_BT_ENABLED
    static BleLink link(*c.config, *c.registry);
    link_ = &link;
#endif

    // ---- 显示屏：探测面板地址，成功则创建 VocDisplay，失败则 NoDisplay ----
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

Board& GetBoard(NodeContext& ctx)
{
    static C3OledVocBoard board(ctx);
    return board;
}

} // namespace esp32node
