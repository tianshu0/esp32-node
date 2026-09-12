#include "app_config/AppConfig.hpp"

#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

namespace esp32node {

static const char* TAG = "app_config";
static constexpr const char* kNamespace = "esp32node";

// NVS 键名（长度必须 <= 15）
static constexpr const char* kKeyNodeId     = "node_id";
static constexpr const char* kKeyI2cSda     = "i2c_sda";
static constexpr const char* kKeyI2cScl     = "i2c_scl";
static constexpr const char* kKeyInterval   = "interval";
static constexpr const char* kKeyHubId      = "hub_id";
static constexpr const char* kKeyPowerSave  = "pwr_save";
static constexpr const char* kKeySeaLevel   = "sea_level";

AppConfig::~AppConfig()
{
    if (open_) {
        nvs_close(handle_);
    }
}

esp_err_t AppConfig::Init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区损坏/版本升级：擦除后重试
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(kNamespace, NVS_READWRITE, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    open_ = true;

    err = Load();
    if (err != ESP_OK) {
        return err;
    }
    EnsureNodeId();
    return ESP_OK;
}

esp_err_t AppConfig::Load()
{
    if (!open_) {
        return ESP_ERR_INVALID_STATE;
    }

    node_id_ = ReadString(handle_, kKeyNodeId, "");
    hub_id_  = ReadString(handle_, kKeyHubId, "");

    uint8_t sda = static_cast<uint8_t>(kDefaultSda);
    if (nvs_get_u8(handle_, kKeyI2cSda, &sda) != ESP_OK) {
        sda = static_cast<uint8_t>(kDefaultSda);
    }
    uint8_t scl = static_cast<uint8_t>(kDefaultScl);
    if (nvs_get_u8(handle_, kKeyI2cScl, &scl) != ESP_OK) {
        scl = static_cast<uint8_t>(kDefaultScl);
    }
    i2c_sda_ = sda;
    i2c_scl_ = scl;

    uint32_t interval = kDefaultIntervalMs;
    if (nvs_get_u32(handle_, kKeyInterval, &interval) != ESP_OK || interval == 0) {
        interval = kDefaultIntervalMs;
    }
    report_interval_ms_ = interval;

    uint8_t power_save = 0;
    if (nvs_get_u8(handle_, kKeyPowerSave, &power_save) != ESP_OK) {
        power_save = 0;
    }
    power_save_ = (power_save != 0);

    float sea_level = 1013.25f;
    size_t len = sizeof(sea_level);
    if (nvs_get_blob(handle_, kKeySeaLevel, &sea_level, &len) != ESP_OK) {
        sea_level = 1013.25f;
    }
    sea_level_hpa_ = sea_level;
    return ESP_OK;
}

std::string AppConfig::ReadString(nvs_handle_t handle, const char* key, const std::string& fallback) const
{
    // 第一次调用返回长度（含结尾 '\0'）
    size_t len = 0;
    if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK) {
        return fallback;
    }
    if (len <= 1) {
        return fallback; // 空串或无数据
    }
    std::string value(len - 1, '\0');
    len = value.size() + 1; // 传入缓冲容量（含结尾 '\0'）
    if (nvs_get_str(handle, key, &value[0], &len) == ESP_OK) {
        return value; // 不含结尾 '\0'
    }
    return fallback;
}

esp_err_t AppConfig::WriteString(const char* key, const std::string& value)
{
    if (!open_) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_str(handle_, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle_);
    }
    return err;
}

void AppConfig::EnsureNodeId()
{
    if (!node_id_.empty()) {
        return;
    }
    // 首次启动：基于 WiFi/BT 基址 MAC 生成，如 "node-1A2B"
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "node-%02X%02X", mac[4], mac[5]);
    node_id_ = buf;

    if (WriteString(kKeyNodeId, node_id_) == ESP_OK) {
        ESP_LOGI(TAG, "generated node_id: %s", node_id_.c_str());
    } else {
        ESP_LOGW(TAG, "persist node_id failed, using volatile id %s", node_id_.c_str());
    }
}

esp_err_t AppConfig::SetNodeId(const std::string& id)
{
    if (id.empty() || id.size() > 31) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = WriteString(kKeyNodeId, id);
    if (err == ESP_OK) {
        node_id_ = id;
    }
    return err;
}

esp_err_t AppConfig::SetI2cPins(int sda, int scl)
{
    if (!open_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sda < 0 || sda > 48 || scl < 0 || scl > 48 || sda == scl) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_set_u8(handle_, kKeyI2cSda, static_cast<uint8_t>(sda));
    if (err == ESP_OK) {
        err = nvs_set_u8(handle_, kKeyI2cScl, static_cast<uint8_t>(scl));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle_);
    }
    if (err == ESP_OK) {
        i2c_sda_ = sda;
        i2c_scl_ = scl;
    }
    return err;
}

esp_err_t AppConfig::SetReportIntervalMs(uint32_t ms)
{
    if (!open_ || ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_set_u32(handle_, kKeyInterval, ms);
    if (err == ESP_OK) {
        err = nvs_commit(handle_);
    }
    if (err == ESP_OK) {
        report_interval_ms_ = ms;
    }
    return err;
}

esp_err_t AppConfig::SetHubId(const std::string& id)
{
    if (id.empty() || id.size() > 31) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = WriteString(kKeyHubId, id);
    if (err == ESP_OK) {
        hub_id_ = id;
    }
    return err;
}

esp_err_t AppConfig::SetPowerSave(bool on)
{
    if (!open_) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u8(handle_, kKeyPowerSave, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle_);
    }
    if (err == ESP_OK) {
        power_save_ = on;
    }
    return err;
}

esp_err_t AppConfig::SetSeaLevelHpa(float hpa)
{
    if (!open_ || hpa < 800.0f || hpa > 1200.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_set_blob(handle_, kKeySeaLevel, &hpa, sizeof(hpa));
    if (err == ESP_OK) {
        err = nvs_commit(handle_);
    }
    if (err == ESP_OK) {
        sea_level_hpa_ = hpa;
    }
    return err;
}

} // namespace esp32node
