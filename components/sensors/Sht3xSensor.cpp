#include "sensors/Sht3xSensor.hpp"

#include <cstdio>
#include <cstring>
#include "i2c_bus/I2cBus.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp32node {

static const char* TAG = "sht3x";

// 单次测量：高重复度、不启用时钟拉伸（测量期间主机等待 ~15ms）
static constexpr uint16_t kCmdMeasureHighNoStretch = 0x2400;
static constexpr uint16_t kCmdSoftReset = 0x30A2;
// 高重复度测量最长时间（datasheet: 12.5ms typ / 15ms max）+ 余量
static constexpr uint32_t kMeasureWaitMs = 20;

// CRC-8：多项式 0x31（x^8 + x^5 + x^4 + 1），初值 0xFF，不做最终异或
static uint8_t Crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

esp_err_t Sht3xSensor::Start(HardwareContext& hw, AppConfig& /*config*/,
                             SensorRegistry& registry)
{
    if (hw.i2c == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = hw.i2c->AddDevice(kAddrDefault, &dev_);
    if (err != ESP_OK) {
        return err;
    }

    err = SendCommand(kCmdSoftReset);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "soft reset failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    // 本地屏幕字段：T（温度）/ H（湿度）。线上 format_json 保持原协议不变。
    static const SensorField kFields[] = {
        {"temp",     "T", "\xC2\xB0" "C", 1},
        {"humidity", "H", "%",            1},
    };
    registry.Register(Type(), "SHT3X",
                      "{\"temp\":\"float\",\"humidity\":\"float\",\"unit\":\"C/%\"}",
                      kFields, sizeof(kFields) / sizeof(kFields[0]),
                      &Sht3xSensor::ReadThunk, this);

    ESP_LOGI(TAG, "sht3x ready at 0x%02X", kAddrDefault);
    return ESP_OK;
}

esp_err_t Sht3xSensor::SendCommand(uint16_t cmd)
{
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {static_cast<uint8_t>(cmd >> 8), static_cast<uint8_t>(cmd & 0xFF)};
    return i2c_master_transmit(dev_, buf, sizeof(buf), kI2cTimeoutMs);
}

bool Sht3xSensor::Read(float* temperature_c, float* humidity_pct)
{
    if (dev_ == nullptr) {
        return false;
    }
    if (SendCommand(kCmdMeasureHighNoStretch) != ESP_OK) {
        ESP_LOGW(TAG, "measure command failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(kMeasureWaitMs));

    uint8_t raw[6] = {};
    if (i2c_master_receive(dev_, raw, sizeof(raw), kI2cTimeoutMs) != ESP_OK) {
        ESP_LOGW(TAG, "read failed");
        return false;
    }
    if (Crc8(raw, 2) != raw[2] || Crc8(raw + 3, 2) != raw[5]) {
        ESP_LOGW(TAG, "crc mismatch");
        return false;
    }

    uint16_t raw_t = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    uint16_t raw_h = static_cast<uint16_t>((raw[3] << 8) | raw[4]);
    if (temperature_c != nullptr) {
        *temperature_c = -45.0f + 175.0f * static_cast<float>(raw_t) / 65535.0f;
    }
    if (humidity_pct != nullptr) {
        *humidity_pct = 100.0f * static_cast<float>(raw_h) / 65535.0f;
    }
    return true;
}

bool Sht3xSensor::ReadThunk(void* ctx, SensorReading* out)
{
    Sht3xSensor* self = static_cast<Sht3xSensor*>(ctx);
    if (self == nullptr || out == nullptr) {
        return false;
    }
    float t = 0.0f;
    float h = 0.0f;
    if (!self->Read(&t, &h)) {
        return false;
    }
    out->ts_ms = esp_timer_get_time() / 1000;
    std::snprintf(out->values_json, sizeof(out->values_json),
                  "{\"temp\":%.2f,\"humidity\":%.2f}",
                  static_cast<double>(t), static_cast<double>(h));
    return true;
}

} // namespace esp32node
