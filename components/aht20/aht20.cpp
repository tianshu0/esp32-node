#include "aht20.hpp"

#include <cstdio>
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
static constexpr uint32_t kI2cTimeoutMs    = 100;

// CRC-8：多项式 0x31（x^8 + x^5 + x^4 + 1），初值 0xFF，不做最终异或。
// AHT20 测量应答共 7 字节：状态 + 5 数据字节 + CRC，CRC 覆盖前 6 字节。
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

Aht20::Aht20(i2c_master_bus_handle_t bus, uint8_t addr)
    : bus_(bus), addr_(addr)
{
}

Aht20::~Aht20()
{
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t Aht20::Init()
{
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr_;
    dev_cfg.scl_speed_hz = 400000;
    esp_err_t err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device 0x%02X failed: %s", addr_, esp_err_to_name(err));
        return err;
    }

    // 上电稳定时间（Init 通常在系统初始化早期，保守等待）
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

    ESP_LOGI(TAG, "aht20 ready at 0x%02X (status=0x%02X)", addr_, status);
    return ESP_OK;
}

esp_err_t Aht20::Transmit(const uint8_t* data, size_t len)
{
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev_, data, len, kI2cTimeoutMs);
}

bool Aht20::ReadStatus(uint8_t* status)
{
    if (status == nullptr || dev_ == nullptr) {
        return false;
    }
    return i2c_master_receive(dev_, status, 1, kI2cTimeoutMs) == ESP_OK;
}

bool Aht20::Read(float* temperature_c, float* humidity_pct)
{
    if (Transmit(kCmdMeasure, sizeof(kCmdMeasure)) != ESP_OK) {
        ESP_LOGW(TAG, "trigger measure failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(kMeasureWaitMs));

    // 7 字节：状态 + 湿度/温度 5 字节 + CRC8（CRC 覆盖前 6 字节）
    uint8_t raw[7] = {};
    if (i2c_master_receive(dev_, raw, sizeof(raw), kI2cTimeoutMs) != ESP_OK) {
        ESP_LOGW(TAG, "read failed");
        return false;
    }
    if (raw[0] & 0x80) {   // bit7=busy，转换未完成
        ESP_LOGW(TAG, "sensor busy");
        return false;
    }
    if (Crc8(raw, 6) != raw[6]) {
        ESP_LOGW(TAG, "crc mismatch: %02X %02X %02X %02X %02X %02X crc=%02X",
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);
        return false;
    }

    const uint32_t raw_h = (static_cast<uint32_t>(raw[1]) << 12) |
                           (static_cast<uint32_t>(raw[2]) << 4) |
                           (static_cast<uint32_t>(raw[3]) >> 4);
    const uint32_t raw_t = (static_cast<uint32_t>(raw[3] & 0x0F) << 16) |
                           (static_cast<uint32_t>(raw[4]) << 8) |
                           static_cast<uint32_t>(raw[5]);

    float h = static_cast<float>(raw_h) * 100.0f / 1048576.0f;
    float t = static_cast<float>(raw_t) * 200.0f / 1048576.0f - 50.0f;
    if (humidity_pct != nullptr) {
        *humidity_pct = h;
    }
    if (temperature_c != nullptr) {
        *temperature_c = t;
    }

    // 诊断日志（10s 限频）
    static int64_t s_last_dbg_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_dbg_us >= 10 * 1000 * 1000) {
        s_last_dbg_us = now_us;
        ESP_LOGI(TAG, "raw=%02X%02X%02X%02X%02X -> T=%.2fC H=%.2f%%",
                 raw[1], raw[2], raw[3], raw[4], raw[5],
                 static_cast<double>(t), static_cast<double>(h));
    }
    return true;
}

} // namespace esp32node
