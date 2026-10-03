#include "bmp280.hpp"

#include <cmath>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp32node {

static const char* TAG = "bmp280";

static constexpr uint8_t kRegChipId   = 0xD0;
static constexpr uint8_t kRegReset    = 0xE0;
static constexpr uint8_t kRegCtrlMeas = 0xF4;
static constexpr uint8_t kRegCalib    = 0x88;   // 24 字节校准系数起始
static constexpr uint8_t kRegData     = 0xF7;   // press[3] + temp[3]
static constexpr uint8_t kChipId      = 0x58;
// T×1 / P×1 / Forced mode：每次 Read 触发一次转换（约 4ms）
static constexpr uint8_t kCtrlForced = 0x25;   // osrs_t=001, osrs_p=001, mode=01
static constexpr uint32_t kI2cTimeoutMs = 100;

static int16_t Le16s(const uint8_t* p)
{
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1]) << 8));
}
static uint16_t Le16u(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8));
}

Bmp280::Bmp280(i2c_master_bus_handle_t bus)
    : bus_(bus)
{
}

Bmp280::~Bmp280()
{
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t Bmp280::Init(float sea_level_hpa)
{
    sea_level_hpa_ = sea_level_hpa;

    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // 地址自识别：依次 Probe 0x76（SDO 接地）/ 0x77（SDO 接高）。
    const uint8_t candidates[] = {kAddrPrimary, kAddrSecondary};
    for (uint8_t a : candidates) {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = a;
        dev_cfg.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus_, &dev_cfg, &dev_) == ESP_OK) {
            addr_ = a;
            break;
        }
    }
    if (addr_ == 0) {
        ESP_LOGE(TAG, "no device answering at 0x%02X or 0x%02X",
                 kAddrPrimary, kAddrSecondary);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t chip = 0;
    if (!ReadRegs(kRegChipId, &chip, 1)) {
        ESP_LOGE(TAG, "read chip id failed at 0x%02X", addr_);
        return ESP_FAIL;
    }
    if (chip != kChipId) {
        // 打印实际 ID 便于辨别贴牌芯片（BME280=0x60, BMP180=0x55 等）
        ESP_LOGE(TAG, "unexpected chip id 0x%02X at 0x%02X (expect BMP280 0x%02X)",
                 chip, addr_, kChipId);
        return ESP_ERR_NOT_FOUND;
    }

    if (!WriteReg(kRegReset, 0xB6)) {
        ESP_LOGE(TAG, "soft reset failed");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t calib[24] = {};
    if (!ReadRegs(kRegCalib, calib, sizeof(calib))) {
        ESP_LOGE(TAG, "read calibration failed");
        return ESP_FAIL;
    }
    dig_t1_ = Le16u(calib + 0);
    dig_t2_ = Le16s(calib + 2);
    dig_t3_ = Le16s(calib + 4);
    dig_p1_ = Le16u(calib + 6);
    dig_p2_ = Le16s(calib + 8);
    dig_p3_ = Le16s(calib + 10);
    dig_p4_ = Le16s(calib + 12);
    dig_p5_ = Le16s(calib + 14);
    dig_p6_ = Le16s(calib + 16);
    dig_p7_ = Le16s(calib + 18);
    dig_p8_ = Le16s(calib + 20);
    dig_p9_ = Le16s(calib + 22);

    ESP_LOGI(TAG, "bmp280 ready at 0x%02X (t1=%u p1=%u)", addr_, dig_t1_, dig_p1_);
    return ESP_OK;
}

bool Bmp280::ReadRegs(uint8_t reg, uint8_t* buf, size_t len)
{
    if (buf == nullptr || dev_ == nullptr) {
        return false;
    }
    return i2c_master_transmit_receive(dev_, &reg, 1, buf, len, kI2cTimeoutMs) == ESP_OK;
}

bool Bmp280::WriteReg(uint8_t reg, uint8_t value)
{
    if (dev_ == nullptr) {
        return false;
    }
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev_, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
}

bool Bmp280::ReadAdc(int32_t* adc_t, int32_t* adc_p)
{
    if (!WriteReg(kRegCtrlMeas, kCtrlForced)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(6));   // T×1+P×1 转换最长约 4.3ms

    uint8_t d[6] = {};
    if (!ReadRegs(kRegData, d, sizeof(d))) {
        return false;
    }
    *adc_p = (static_cast<int32_t>(d[0]) << 12) |
             (static_cast<int32_t>(d[1]) << 4) |
             (static_cast<int32_t>(d[2]) >> 4);
    *adc_t = (static_cast<int32_t>(d[3]) << 12) |
             (static_cast<int32_t>(d[4]) << 4) |
             (static_cast<int32_t>(d[5]) >> 4);
    return true;
}

// Bosch datasheet 4.2.3 浮点补偿算法
void Bmp280::Compute(int32_t adc_t, int32_t adc_p,
                     float* out_t, float* out_p) const
{
    double var1 = (static_cast<double>(adc_t) / 16384.0 -
                   static_cast<double>(dig_t1_) / 1024.0) *
                  static_cast<double>(dig_t2_);
    double var2_t = (static_cast<double>(adc_t) / 131072.0 -
                     static_cast<double>(dig_t1_) / 8192.0);
    var2_t = var2_t * var2_t * static_cast<double>(dig_t3_);
    const double t_fine = var1 + var2_t;
    *out_t = static_cast<float>(t_fine / 5120.0);

    var1 = t_fine / 2.0 - 64000.0;
    double var2 = var1 * var1 * static_cast<double>(dig_p6_) / 32768.0;
    var2 = var2 + var1 * static_cast<double>(dig_p5_) * 2.0;
    var2 = var2 / 4.0 + static_cast<double>(dig_p4_) * 65536.0;
    var1 = (static_cast<double>(dig_p3_) * var1 * var1 / 524288.0 +
            static_cast<double>(dig_p2_) * var1) / 524288.0;
    var1 = (1.0 + var1 / 32768.0) * static_cast<double>(dig_p1_);
    if (var1 == 0.0) {
        *out_p = 0.0f;
        return;
    }
    double p = 1048576.0 - static_cast<double>(adc_p);
    p = (p - var2 / 4096.0) * 6250.0 / var1;
    var1 = static_cast<double>(dig_p9_) * p * p / 2147483648.0;
    var2 = p * static_cast<double>(dig_p8_) / 32768.0;
    p = p + (var1 + var2 + static_cast<double>(dig_p7_)) / 16.0;
    *out_p = static_cast<float>(p);   // Pa
}

bool Bmp280::Read(float* temperature_c, float* pressure_hpa, float* altitude_m)
{
    int32_t adc_t = 0;
    int32_t adc_p = 0;
    if (!ReadAdc(&adc_t, &adc_p)) {
        ESP_LOGW(TAG, "read adc failed");
        return false;
    }

    float t = 0.0f;
    float p_pa = 0.0f;
    Compute(adc_t, adc_p, &t, &p_pa);
    if (p_pa <= 0.0f) {
        ESP_LOGW(TAG, "invalid pressure result");
        return false;
    }

    const float hpa = p_pa / 100.0f;
    if (temperature_c != nullptr) {
        *temperature_c = t;
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

} // namespace esp32node
