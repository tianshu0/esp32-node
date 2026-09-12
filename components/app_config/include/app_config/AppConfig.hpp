// app_config 组件：基于 NVS 的持久化配置存取
//
// 本组件提供节点 ID、I2C 引脚、上报间隔、已配对 hub、低功耗开关等配置项读写。
//
// 与 esp32-hub 的 AppConfig 区别：
//   - 不需要 Wi-Fi / MQTT 字段（节点不联网，只做 BLE 外设）
//   - 多了 I2C 引脚配置（两颗传感器共用一条总线）
//   - 多了 hub_id（握手中从 hello 提取，仅作记录）
#pragma once

#include <string>
#include <cstdint>
#include "esp_err.h"
#include "nvs.h"

namespace esp32node {

class AppConfig {
public:
    // 默认值：ESP32-C3 mini 上 4/5 均为普通 GPIO
    // （避开 strapping 2/8/9、片内 Flash 11~17、USB 18/19、UART0 20/21）
    static constexpr int kDefaultSda = 4;
    static constexpr int kDefaultScl = 5;
    static constexpr uint32_t kDefaultIntervalMs = 5000;

    AppConfig() = default;
    ~AppConfig();

    // 初始化 NVS（内部 nvs_flash_init，并对首启/损坏做 erase 重试），
    // 首次启动会基于 MAC 生成并保存 node_id
    esp_err_t Init();

    // ---- 节点 ID（形如 "node-1A2B"）----
    const std::string& NodeId() const { return node_id_; }
    esp_err_t SetNodeId(const std::string& id);

    // ---- I2C 总线引脚（SHT3X 与 BMP180 共用）----
    int I2cSda() const { return i2c_sda_; }
    int I2cScl() const { return i2c_scl_; }
    esp_err_t SetI2cPins(int sda, int scl);

    // ---- 数据上报间隔（握手第 3 步由 hub 下发）----
    uint32_t ReportIntervalMs() const { return report_interval_ms_; }
    esp_err_t SetReportIntervalMs(uint32_t ms);

    // ---- 已配对的 hub ID（握手第 1 步从 hello 提取）----
    const std::string& HubId() const { return hub_id_; }
    esp_err_t SetHubId(const std::string& id);

    // ---- 低功耗开关（采集间隙自动 light sleep，默认关闭）----
    bool PowerSave() const { return power_save_; }
    esp_err_t SetPowerSave(bool on);

    // ---- 海拔换算的海平面气压参考值（hPa）----
    float SeaLevelHpa() const { return sea_level_hpa_; }
    esp_err_t SetSeaLevelHpa(float hpa);

    // 重新从 NVS 加载全部配置
    esp_err_t Load();

private:
    std::string ReadString(nvs_handle_t handle, const char* key, const std::string& fallback) const;
    esp_err_t WriteString(const char* key, const std::string& value);
    void EnsureNodeId();

    nvs_handle_t handle_ = 0;
    bool open_ = false;

    std::string node_id_;
    int i2c_sda_ = kDefaultSda;
    int i2c_scl_ = kDefaultScl;
    uint32_t report_interval_ms_ = kDefaultIntervalMs;
    std::string hub_id_;
    bool power_save_ = false;
    float sea_level_hpa_ = 1013.25f;
};

} // namespace esp32node
