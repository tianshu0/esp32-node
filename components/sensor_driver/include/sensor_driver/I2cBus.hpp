// I2cBus：I2C 主机总线封装（ESP-IDF 新版 i2c_master 驱动）
//
// SHT3X 与 BMP180 共用同一条 I2C 总线，总线由 sensor_driver 统一持有，
// 各传感器驱动只从总线拿到自己的设备句柄。
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/i2c_master.h"

namespace esp32node {

class I2cBus {
public:
    I2cBus() = default;
    ~I2cBus();

    // 创建 I2C 主机总线（SDA/SCL 由 app_config 提供）
    esp_err_t Init(int sda_gpio, int scl_gpio, uint32_t clk_hz = 100000);

    // 在总线上挂载一个从设备，返回设备句柄
    esp_err_t AddDevice(uint8_t addr_7bit, i2c_master_dev_handle_t* out);

    // 探测设备是否应答（用于上电自检）
    bool Probe(uint8_t addr_7bit);

    i2c_master_bus_handle_t Handle() const { return bus_; }

private:
    static constexpr uint32_t kProbeTimeoutMs = 100;

    i2c_master_bus_handle_t bus_ = nullptr;
    uint32_t clk_hz_ = 100000;
};

} // namespace esp32node
