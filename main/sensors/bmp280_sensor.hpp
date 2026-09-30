// BMP280 气压/温度传感器驱动（I2C）—— SensorDevice 插件实现
//
// - 7 位地址：0x76（SDO 接地，多数模块默认）或 0x77（SDO 接高），
//   Start() 会依次 Probe 两个地址自动识别
// - 芯片 ID：寄存器 0xD0 读回 0x58
// - 校准系数：0x88 起 24 字节（T1~T3 3 个 + P1~P9 9 个，共 12 个 16bit 系数）
// - 单次测量：ctrl_meas 写 0x25（T×1 P×1 Forced），约 4ms，回读 6 字节
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "sensors/sensor_device.hpp"
#include "sensor_registry/sensor_registry.hpp"

namespace esp32node {

class Bmp280Sensor : public SensorDevice {
public:
    static constexpr uint8_t kAddrPrimary   = 0x76;  // SDO 接地
    static constexpr uint8_t kAddrSecondary = 0x77;  // SDO 接高

    const char* Type() const override { return "pressure"; }
    esp_err_t Start(Board& board, AppConfig& config,
                    SensorRegistry& registry) override;

    bool Read(float* temperature_c, float* pressure_hpa, float* altitude_m = nullptr);

    static bool ReadThunk(void* ctx, SensorReading* out);
    void SetSeaLevelHpa(float hpa) { sea_level_hpa_ = hpa; }

private:
    bool ReadRegs(uint8_t reg, uint8_t* buf, size_t len);
    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadAdc(int32_t* adc_t, int32_t* adc_p);
    void Compute(int32_t adc_t, int32_t adc_p, float* out_t, float* out_p) const;

    Board* board_ = nullptr;
    uint8_t addr_ = 0;   // 实际探测到的地址（0x76 / 0x77）
    float sea_level_hpa_ = 1013.25f;

    uint16_t dig_t1_ = 0;   // T1 无符号（datasheet），T2/T3 有符号
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

} // namespace esp32node
