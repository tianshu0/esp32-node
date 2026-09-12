// BMP180（GY-68）气压/温度传感器驱动（I2C）
//
// - 7 位地址：0x77（GY-68 模块固定，SDO 悬空）
// - 芯片 ID：寄存器 0xD0 读回 0x55
// - 校准系数：0xAA 起 22 字节（11 个 int16，大端）
// - 温度：0xF4 写 0x2E，等待 4.5ms 后从 0xF6 读 2 字节
// - 气压：0xF4 写 0x34|(oss<<6)，等待后从 0xF6 读 3 字节（oss 越大越准越慢）
// - 海拔：h = 44330 * (1 - (p/p0)^(1/5.255))，p0 为海平面气压参考值
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "sensor_registry/SensorRegistry.hpp"
#include "driver/i2c_master.h"

namespace esp32node {

class I2cBus;

class Bmp180 {
public:
    static constexpr uint8_t kAddr = 0x77;
    // 过采样精度（oss）：0=1 次 1=2 次 2=4 次 3=8 次
    static constexpr uint8_t kOversampling = 1;

    Bmp180() = default;

    // 挂载设备、校验芯片 ID、读取出厂校准系数
    esp_err_t Init(I2cBus& bus, uint8_t addr = kAddr);

    // 采集：温度（℃）、气压（hPa）、海拔（m，可传 nullptr 跳过）
    bool Read(float* temperature_c, float* pressure_hpa, float* altitude_m = nullptr);

    // 供 SensorRegistry 使用的统一签名
    static bool ReadThunk(void* ctx, SensorReading* out);

    void SetSeaLevelHpa(float hpa) { sea_level_hpa_ = hpa; }

private:
    static constexpr uint32_t kI2cTimeoutMs = 100;

    bool ReadRegs(uint8_t reg, uint8_t* buf, size_t len);
    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadRawTemp(int32_t* ut);
    bool ReadRawPressure(int32_t* up);
    // 按 datasheet 的整数算法由原始值算出温度（0.1℃）与气压（Pa）
    void Compute(int32_t ut, int32_t up, int32_t* temp_0c1, int32_t* pressure_pa) const;

    i2c_master_dev_handle_t dev_ = nullptr;

    // 出厂校准系数（int16 有符号 / 无符号按 datasheet）
    int16_t ac1_ = 0;
    int16_t ac2_ = 0;
    int16_t ac3_ = 0;
    uint16_t ac4_ = 0;
    uint16_t ac5_ = 0;
    uint16_t ac6_ = 0;
    int16_t b1_ = 0;
    int16_t b2_ = 0;
    int16_t mb_ = 0;
    int16_t mc_ = 0;
    int16_t md_ = 0;

    float sea_level_hpa_ = 1013.25f;
};

} // namespace esp32node
