// 项目装配：ESP32-C3 + SSD1315 128x64 OLED + SHT3X 温湿度 + BMP180 气压
//
// Assemble()：建 I2C 总线 -> 实例化传感器 -> 探测面板地址并创建 ThpDisplay
#include "c3_oled_thp_board.hpp"
#include "thp_display.hpp"
#include "config.h"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"

#include "sensors/sensor_device.hpp"
#include "sensors/sht3x_sensor.hpp"
#include "sensors/bmp180_sensor.hpp"

#include "link/ble_link.hpp"
#include "display/no_display.hpp"

#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "board-thp";

C3OledThpBoard::C3OledThpBoard(NodeContext& ctx)
    : Board(ctx)
{
    display_ = nullptr;  // 需要 I2C 总线，延迟到 Assemble 创建
}

i2c_master_dev_handle_t C3OledThpBoard::I2cDevice(uint8_t addr)
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

esp_err_t C3OledThpBoard::I2cWrite(uint8_t addr, const uint8_t* data, size_t len)
{
    i2c_master_dev_handle_t dev = I2cDevice(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev, data, len, kI2cTimeoutMs);
}

esp_err_t C3OledThpBoard::I2cRead(uint8_t addr, uint8_t* buf, size_t len)
{
    i2c_master_dev_handle_t dev = I2cDevice(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_receive(dev, buf, len, kI2cTimeoutMs);
}

esp_err_t C3OledThpBoard::I2cWriteRead(uint8_t addr, const uint8_t* w, size_t wlen,
                                       uint8_t* r, size_t rlen)
{
    i2c_master_dev_handle_t dev = I2cDevice(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, w, wlen, r, rlen, kI2cTimeoutMs);
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
    bus_cfg.flags.enable_internal_pullup = true;  // 模块板载已有上拉，再开内部上拉提高容错
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus_));

    // ---- 传感器：驱动经本板的 I2C 原语访问总线，Start() 内自登记 ----
    static Sht3xSensor sht3x;
    if (sht3x.Start(*this, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "sht3x disabled, check wiring (addr 0x44/0x45)");
    }

    static Bmp180Sensor bmp180;
    if (bmp180.Start(*this, *c.config, *c.registry) != ESP_OK) {
        ESP_LOGW(TAG, "bmp180 disabled, check wiring (addr 0x77)");
    }

    // ---- 链路：BLE 外设（本板是 BLE 节点，经 BLE 与 hub 握手/周期上报）----
#if CONFIG_BT_ENABLED
    static BleLink link(*c.config, *c.registry);
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

Board& GetBoard(NodeContext& ctx)
{
    static C3OledThpBoard board(ctx);
    return board;
}

} // namespace esp32node
