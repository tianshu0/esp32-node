// BMP280 气压/温度传感器驱动（I2C）—— 独立可移植组件
//
// 仅依赖 ESP-IDF：driver/i2c_master.h
// 板级负责创建 i2c_master_bus_handle_t 并传入；本组件自行挂载设备句柄，
// 并在 Init 内自动探测 0x76/0x77 两个地址。
//
// - 7 位地址：0x76（SDO 接地，多数模块默认）或 0x77（SDO 接高）
// - 芯片 ID：寄存器 0xD0 读回 0x58
// - 校准系数：0x88 起 24 字节（T1~T3 3 个 + P1~P9 9 个，共 12 个 16bit 系数）
// - 单次测量：ctrl_meas 写 0x25（T×1 P×1 Forced），约 4ms，回读 6 字节
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/i2c_master.h"

class Bmp280 {
public:
    static constexpr uint8_t kAddrPrimary   = 0x76;  // SDO 接地
    static constexpr uint8_t kAddrSecondary = 0x77;  // SDO 接高

    // 构造时仅保存总线句柄；实际地址探测与设备挂载在 Init 内完成。
    explicit Bmp280(i2c_master_bus_handle_t bus);
    ~Bmp280();

    // 探测地址 -> 读芯片 ID -> 软复位 -> 读校准系数。sea_level_hpa 用于海拔换算。
    esp_err_t Init(float sea_level_hpa = 1013.25f);

    // 采集：温度（℃）、气压（hPa）、海拔（m，可传 nullptr 跳过）
    bool Read(float* temperature_c, float* pressure_hpa, float* altitude_m = nullptr);

    void SetSeaLevelHpa(float hpa) { sea_level_hpa_ = hpa; }
    uint8_t Address() const { return addr_; }

private:
    bool ReadRegs(uint8_t reg, uint8_t* buf, size_t len);
    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadAdc(int32_t* adc_t, int32_t* adc_p);
    void Compute(int32_t adc_t, int32_t adc_p, float* out_t, float* out_p) const;

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t addr_ = 0;   // 实际探测到的地址（0x76 / 0x77）
    float sea_level_hpa_ = 1013.25f;

    uint16_t dig_t1_ = 0;
    int16_t dig_t2_ = 0;
    int16_t dig_t3_ = 0;
    uint16_t dig_p1_ = 0;
    int16_t dig_p2_ = 0;
    int16_t dig_p3_ = 0;
    int16_t dig_p4_ = 0;
    int16_t dig_p5_ = 0;
    int16_t dig_p6_ = 0;
    int16_t dig_p7_ = 0;
    int16_t dig_p8_ = 0;
    int16_t dig_p9_ = 0;
};
