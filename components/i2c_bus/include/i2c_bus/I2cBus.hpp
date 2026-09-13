// i2c_bus 组件：I2C 主机总线封装（ESP-IDF 新版 i2c_master 驱动）
//
// 由 main 持有，SHT3X / BMP180（sensor_driver）与 SSD1315 OLED（oled_display）
// 共用同一条总线，各设备只从总线拿到自己的设备句柄。
//
// 默认 400kHz：整屏 OLED 刷新（1KB + 控制字节）在 100kHz 下约 90ms，
// 会明显拖住 LVGL 刷新任务；400kHz 下约 23ms。SHT3X（≤1MHz）与
// BMP180（≤3.4MHz）均支持 400kHz。
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esp32node {

class I2cBus {
public:
    static constexpr uint32_t kDefaultClkHz = 400000;

    I2cBus() = default;
    ~I2cBus();

    // 创建 I2C 主机总线（SDA/SCL 由 app_config 提供）
    esp_err_t Init(int sda_gpio, int scl_gpio, uint32_t clk_hz = kDefaultClkHz);

    // 在总线上挂载一个从设备，返回设备句柄
    esp_err_t AddDevice(uint8_t addr_7bit, i2c_master_dev_handle_t* out);

    // 探测设备是否应答（用于上电自检 / 屏地址 0x3C|0x3D 自动选择）
    bool Probe(uint8_t addr_7bit);

    // 把「多笔事务组成的一次完整访问」原子化。
    // 说明：i2c_master 驱动本身已对总线加锁，单笔传输线程安全；这里的锁用于
    // 避免「传感器整段读取（写命令 + 等待 + 读回）」与「OLED 整屏刷新」交错，
    // 让 display 与 sensor 两条任务在总线占用上互不插入。
    void Lock();
    void Unlock();

    i2c_master_bus_handle_t Handle() const { return bus_; }

private:
    static constexpr uint32_t kProbeTimeoutMs = 100;

    i2c_master_bus_handle_t bus_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    uint32_t clk_hz_ = kDefaultClkHz;
};

} // namespace esp32node
