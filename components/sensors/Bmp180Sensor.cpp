#include "sensors/Bmp180Sensor.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include "i2c_bus/I2cBus.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp32node {

static const char* TAG = "bmp180";

static constexpr uint8_t kRegCalib = 0xAA;   // 22 字节校准系数起始
static constexpr uint8_t kRegChipId = 0xD0;
static constexpr uint8_t kRegCtrl = 0xF4;
static constexpr uint8_t kRegData = 0xF6;
static constexpr uint8_t kChipId = 0x55;
static constexpr uint8_t kCmdReadTemp = 0x2E;

// 各过采样档位的转换等待时间（ms），datasheet: 4.5 / 7.5 / 13.5 / 25.5
static constexpr uint32_t kPressureWaitMs[4] = {5, 8, 14, 26};

static int16_t Be16(const uint8_t* p)
{
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

esp_err_t Bmp180Sensor::Start(HardwareContext& hw, AppConfig& config,
                              SensorRegistry& registry)
{
    if (hw.i2c == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = hw.i2c->AddDevice(kAddr, &dev_);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t chip = 0;
    if (!ReadRegs(kRegChipId, &chip, 1)) {
        ESP_LOGE(TAG, "read chip id failed");
        return ESP_FAIL;
    }
    if (chip != kChipId) {
        ESP_LOGE(TAG, "unexpected chip id 0x%02X (expect 0x%02X)", chip, kChipId);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t calib[22] = {};
    if (!ReadRegs(kRegCalib, calib, sizeof(calib))) {
        ESP_LOGE(TAG, "read calibration failed");
        return ESP_FAIL;
    }
    ac1_ = Be16(calib + 0);
    ac2_ = Be16(calib + 2);
    ac3_ = Be16(calib + 4);
    ac4_ = static_cast<uint16_t>(Be16(calib + 6));
    ac5_ = static_cast<uint16_t>(Be16(calib + 8));
    ac6_ = static_cast<uint16_t>(Be16(calib + 10));
    b1_  = Be16(calib + 12);
    b2_  = Be16(calib + 14);
    mb_  = Be16(calib + 16);
    mc_  = Be16(calib + 18);
    md_  = Be16(calib + 20);

    SetSeaLevelHpa(config.SeaLevelHpa());

    // 本地屏幕只显示气压一行（温度与 SHT3X 重复、海拔占空间，故不登记显示字段）；
    // values_json 仍包含 pressure/temp/altitude 三个值，线上 format_json 不变。
    static const SensorField kFields[] = {
        {"pressure", "P", "hPa", 1},
    };
    registry.Register(Type(), "BMP180",
                      "{\"pressure\":\"float\",\"temp\":\"float\","
                      "\"altitude\":\"float\",\"unit\":\"hPa/C/m\"}",
                      kFields, sizeof(kFields) / sizeof(kFields[0]),
                      &Bmp180Sensor::ReadThunk, this);

    ESP_LOGI(TAG, "bmp180 ready at 0x%02X (ac1=%d ac4=%u ac5=%u)",
             kAddr, ac1_, ac4_, ac5_);
    return ESP_OK;
}

bool Bmp180Sensor::ReadRegs(uint8_t reg, uint8_t* buf, size_t len)
{
    if (dev_ == nullptr || buf == nullptr) {
        return false;
    }
    if (i2c_master_transmit(dev_, &reg, 1, kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    return i2c_master_receive(dev_, buf, len, kI2cTimeoutMs) == ESP_OK;
}

bool Bmp180Sensor::WriteReg(uint8_t reg, uint8_t value)
{
    if (dev_ == nullptr) {
        return false;
    }
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev_, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
}

bool Bmp180Sensor::ReadRawTemp(int32_t* ut)
{
    if (!WriteReg(kRegCtrl, kCmdReadTemp)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));   // 温度转换 4.5ms

    uint8_t d[2] = {};
    if (!ReadRegs(kRegData, d, sizeof(d))) {
        return false;
    }
    *ut = static_cast<int32_t>((static_cast<uint32_t>(d[0]) << 8) | d[1]);
    return true;
}

bool Bmp180Sensor::ReadRawPressure(int32_t* up)
{
    const uint8_t oss = kOversampling;
    if (!WriteReg(kRegCtrl, static_cast<uint8_t>(0x34 | (oss << 6)))) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(kPressureWaitMs[oss & 0x03]));

    uint8_t d[3] = {};
    if (!ReadRegs(kRegData, d, sizeof(d))) {
        return false;
    }
    int32_t raw = (static_cast<int32_t>(d[0]) << 16) |
                  (static_cast<int32_t>(d[1]) << 8) |
                  static_cast<int32_t>(d[2]);
    *up = raw >> (8 - oss);
    return true;
}

void Bmp180Sensor::Compute(int32_t ut, int32_t up, int32_t* temp_0c1, int32_t* pressure_pa) const
{
    // 温度（datasheet 3.5 节整数算法，结果单位 0.1℃）
    int32_t x1 = ((ut - static_cast<int32_t>(ac6_)) * static_cast<int32_t>(ac5_)) >> 15;
    int32_t x2 = (static_cast<int32_t>(mc_) << 11) / (x1 + static_cast<int32_t>(md_));
    const int32_t b5 = x1 + x2;
    *temp_0c1 = (b5 + 8) >> 4;

    // 气压（结果单位 Pa）
    const int32_t b6 = b5 - 4000;
    x1 = (static_cast<int32_t>(b2_) * ((b6 * b6) >> 12)) >> 11;
    x2 = (static_cast<int32_t>(ac2_) * b6) >> 11;
    int32_t x3 = x1 + x2;
    int32_t b3 = (((static_cast<int32_t>(ac1_) * 4 + x3) << kOversampling) + 2) >> 2;

    x1 = (static_cast<int32_t>(ac3_) * b6) >> 13;
    x2 = (static_cast<int32_t>(b1_) * ((b6 * b6) >> 12)) >> 16;
    x3 = ((x1 + x2) + 2) >> 2;
    const uint32_t b4 = (static_cast<uint32_t>(ac4_) * static_cast<uint32_t>(x3 + 32768)) >> 15;
    const uint32_t b7 = (static_cast<uint32_t>(up) - static_cast<uint32_t>(b3)) *
                        static_cast<uint32_t>(50000 >> kOversampling);

    int32_t p;
    if (b4 == 0) {
        *pressure_pa = 0;
        return;
    }
    if (b7 < 0x80000000u) {
        p = static_cast<int32_t>((b7 * 2) / b4);
    } else {
        p = static_cast<int32_t>((b7 / b4) * 2);
    }
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    p = p + ((x1 + x2 + 3791) >> 4);
    *pressure_pa = p;
}

bool Bmp180Sensor::Read(float* temperature_c, float* pressure_hpa, float* altitude_m)
{
    if (dev_ == nullptr) {
        return false;
    }
    int32_t ut = 0;
    int32_t up = 0;
    if (!ReadRawTemp(&ut)) {
        ESP_LOGW(TAG, "read raw temperature failed");
        return false;
    }
    if (!ReadRawPressure(&up)) {
        ESP_LOGW(TAG, "read raw pressure failed");
        return false;
    }

    int32_t temp_0c1 = 0;
    int32_t pressure_pa = 0;
    Compute(ut, up, &temp_0c1, &pressure_pa);
    if (pressure_pa <= 0) {
        ESP_LOGW(TAG, "invalid pressure result");
        return false;
    }

    const float hpa = static_cast<float>(pressure_pa) / 100.0f;
    if (temperature_c != nullptr) {
        *temperature_c = static_cast<float>(temp_0c1) / 10.0f;
    }
    if (pressure_hpa != nullptr) {
        *pressure_hpa = hpa;
    }
    if (altitude_m != nullptr) {
        *altitude_m = 44330.0f *
                      (1.0f - powf(hpa / sea_level_hpa_, 1.0f / 5.255f));
    }
    return true;
}

bool Bmp180Sensor::ReadThunk(void* ctx, SensorReading* out)
{
    Bmp180Sensor* self = static_cast<Bmp180Sensor*>(ctx);
    if (self == nullptr || out == nullptr) {
        return false;
    }
    float t = 0.0f;
    float p = 0.0f;
    float alt = 0.0f;
    if (!self->Read(&t, &p, &alt)) {
        return false;
    }
    out->ts_ms = esp_timer_get_time() / 1000;
    std::snprintf(out->values_json, sizeof(out->values_json),
                  "{\"pressure\":%.2f,\"temp\":%.2f,\"altitude\":%.1f}",
                  static_cast<double>(p), static_cast<double>(t),
                  static_cast<double>(alt));
    return true;
}

} // namespace esp32node
