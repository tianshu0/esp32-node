// s3_tft_fan 板：ESP32-S3 N16R8 + ILI9341 240x320 SPI TFT + XPT2046 触摸
//                + AHT20/BMP280 二合一模块 + PWM 风扇 + 自动化规则
//
// 构造函数：创建 FanDisplay（面板/LVGL/触摸在构造内初始化）
// Assemble()：建 SPI/I2C 总线 -> 实例化传感器/风扇/自动化
#pragma once

#include <cstddef>
#include <cstdint>
#include "boards/board.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class S3TftFanBoard : public Board {
public:
    explicit S3TftFanBoard(NodeContext& ctx);

    void Assemble() override;
    const char* Name() const override { return "s3_tft_fan"; }

    // ---- 板载 I2C 原语（地址式）：AHT20 / BMP280 二合一模块 ----
    esp_err_t I2cWrite(uint8_t addr, const uint8_t* data, size_t len) override;
    esp_err_t I2cRead(uint8_t addr, uint8_t* buf, size_t len) override;
    esp_err_t I2cWriteRead(uint8_t addr, const uint8_t* w, size_t wlen,
                           uint8_t* r, size_t rlen) override;
    bool I2cProbe(uint8_t addr) override;

private:
    static constexpr uint32_t kI2cClkHz = 400000;
    static constexpr int kI2cTimeoutMs = 100;
    static constexpr size_t kMaxI2cDevs = 4;

    struct I2cDev {
        uint8_t addr;
        i2c_master_dev_handle_t dev;
    };

    // i2c_master 以设备句柄寻址，这里按 7 位地址做一次惰性挂载并缓存
    i2c_master_dev_handle_t I2cDevice(uint8_t addr);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    I2cDev i2c_devs_[kMaxI2cDevs] = {};
    size_t i2c_dev_count_ = 0;
};

} // namespace esp32node
