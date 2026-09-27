#include "sensors/Aht20Sensor.hpp"

#include <cstdio>
#include "i2c_bus/I2cBus.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp32node {

static const char* TAG = "aht20";

// 校准/初始化命令与触发测量命令（datasheet 5.3/5.4）
static constexpr uint8_t kCmdInit[3]     = {0xBE, 0x08, 0x00};
static constexpr uint8_t kCmdMeasure[3]  = {0xAC, 0x33, 0x00};
static constexpr uint8_t kCmdSoftReset   = 0xBA;
static constexpr uint32_t kPowerOnWaitMs   = 45;   // 上电稳定 ≥40ms
static constexpr uint32_t kMeasureWaitMs   = 85;   // 转换最长 80ms + 余量

esp_err_t Aht20Sensor::Start(HardwareContext& hw, AppConfig& /*config*/,
                             SensorRegistry& registry)
{
    if (hw.i2c == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = hw.i2c->AddDevice(kAddr, &dev_);
    if (err != ESP_OK) {
        return err;
    }

    // 上电稳定时间（Start 通常在系统初始化早期，保守等待）
    vTaskDelay(pdMS_TO_TICKS(kPowerOnWaitMs));

    // 软复位后再发校准命令，确保芯片处于已知状态
    err = Transmit(&kCmdSoftReset, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "soft reset failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));   // 复位耗时 ≤20ms

    err = Transmit(kCmdInit, sizeof(kCmdInit));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calibrate command failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t status = 0;
    if (!ReadStatus(&status) || (status & 0x08) == 0) {
        ESP_LOGE(TAG, "not calibrated (status=0x%02X)", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 本地屏幕字段：T（温度）/ H（湿度）。线上 format_json 与 SHT3X 保持一致，
    // hub / 自动化 / 显示模板按 type 与字段名取数，无感知替换。
    static const SensorField kFields[] = {
        {"temp",     "T", "\xC2\xB0" "C", 1},
        {"humidity", "H", "%",            1},
    };
    registry.Register(Type(), "AHT20",
                      "{\"temp\":\"float\",\"humidity\":\"float\",\"unit\":\"C/%\"}",
                      kFields, sizeof(kFields) / sizeof(kFields[0]),
                      &Aht20Sensor::ReadThunk, this);

    ESP_LOGI(TAG, "aht20 ready at 0x%02X (status=0x%02X)", kAddr, status);
    return ESP_OK;
}

esp_err_t Aht20Sensor::Transmit(const uint8_t* data, size_t len)
{
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev_, data, len, kI2cTimeoutMs);
}

bool Aht20Sensor::ReadStatus(uint8_t* status)
{
    if (dev_ == nullptr || status == nullptr) {
        return false;
    }
    return i2c_master_receive(dev_, status, 1, kI2cTimeoutMs) == ESP_OK;
}

bool Aht20Sensor::Read(float* temperature_c, float* humidity_pct)
{
    if (dev_ == nullptr) {
        return false;
    }
    if (Transmit(kCmdMeasure, sizeof(kCmdMeasure)) != ESP_OK) {
        ESP_LOGW(TAG, "trigger measure failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(kMeasureWaitMs));

    uint8_t raw[6] = {};
    if (i2c_master_receive(dev_, raw, sizeof(raw), kI2cTimeoutMs) != ESP_OK) {
        ESP_LOGW(TAG, "read failed");
        return false;
    }
    if (raw[0] & 0x80) {   // bit7=busy，转换未完成
        ESP_LOGW(TAG, "sensor busy");
        return false;
    }

    const uint32_t raw_h = (static_cast<uint32_t>(raw[1]) << 12) |
                           (static_cast<uint32_t>(raw[2]) << 4) |
                           (static_cast<uint32_t>(raw[3]) >> 4);
    const uint32_t raw_t = (static_cast<uint32_t>(raw[3] & 0x0F) << 16) |
                           (static_cast<uint32_t>(raw[4]) << 8) |
                           static_cast<uint32_t>(raw[5]);

    if (humidity_pct != nullptr) {
        *humidity_pct = static_cast<float>(raw_h) * 100.0f / 1048576.0f;
    }
    if (temperature_c != nullptr) {
        *temperature_c = static_cast<float>(raw_t) * 200.0f / 1048576.0f - 50.0f;
    }
    return true;
}

bool Aht20Sensor::ReadThunk(void* ctx, SensorReading* out)
{
    Aht20Sensor* self = static_cast<Aht20Sensor*>(ctx);
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
