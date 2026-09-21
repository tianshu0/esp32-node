// Automation 实现：规则引擎 + NVS 持久化 + 迟滞保护
#include "automation/Automation.hpp"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esp32node {

static const char* TAG = "automation";
static constexpr const char* kNvsNs = "automation";
static constexpr const char* kNvsKeyCount = "rule_count";
static constexpr const char* kNvsKeyRules = "rules";

static constexpr uint32_t kCheckIntervalMs = 5000;  // 每 5 秒检查一次

esp_err_t Automation::Init(AppConfig* config, SensorRegistry* registry, FanControl* fan)
{
    config_   = config;
    registry_ = registry;
    fan_      = fan;

    esp_err_t err = LoadRules();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load rules from NVS failed: %s, starting empty",
                 esp_err_to_name(err));
    }

    // 启动规则检查任务
    if (xTaskCreate(&Automation::TaskMain, "automation", 4096, this, 4, &task_) != pdPASS) {
        ESP_LOGE(TAG, "create automation task failed, free heap %u B",
                 (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "automation init: %d rules loaded", count_);
    return ESP_OK;
}

esp_err_t Automation::LoadRules()
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    int32_t saved_count = 0;
    err = nvs_get_i32(h, kNvsKeyCount, &saved_count);
    if (err == ESP_OK && saved_count > 0) {
        size_t buf_size = sizeof(rules_);
        err = nvs_get_blob(h, kNvsKeyRules, rules_, &buf_size);
        if (err == ESP_OK) {
            count_ = std::min(static_cast<int>(saved_count), kMaxRules);
            ESP_LOGI(TAG, "loaded %d rules from NVS", count_);
        }
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t Automation::SaveRule(int idx, const Rule& rule)
{
    if (idx < 0 || idx >= kMaxRules) return ESP_ERR_INVALID_ARG;

    rules_[idx] = rule;
    if (idx >= count_) count_ = idx + 1;

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_i32(h, kNvsKeyCount, count_);
    if (err != ESP_OK) goto out;

    err = nvs_set_blob(h, kNvsKeyRules, rules_, sizeof(rules_));
    if (err != ESP_OK) goto out;

    err = nvs_commit(h);

out:
    nvs_close(h);
    if (err == ESP_OK) {
        const char* op_str[] = {">", "<", ">=", "<="};
        int op_idx = static_cast<int>(rule.op);
        ESP_LOGI(TAG, "rule[%d] saved: %s %s %.1f -> fan %d%% dur %us",
                 idx, rule.sensor_field,
                 (op_idx >= 0 && op_idx <= 3) ? op_str[op_idx] : "?",
                 rule.threshold, rule.fan_power,
                 rule.duration_sec);
    }
    return err;
}

esp_err_t Automation::DisableRule(int idx)
{
    if (idx < 0 || idx >= kMaxRules) return ESP_ERR_INVALID_ARG;
    rules_[idx].enabled = false;
    return SaveRule(idx, rules_[idx]);
}

float Automation::SensorValue(const char* field) const
{
    if (!registry_ || !field) return -1e9f;

    // 让 registry 立即采集所有传感器，然后在 values_json 里查找目标字段
    SensorReading readings[4];
    int n = registry_->ReadAll(readings, 4);
    for (int i = 0; i < n; ++i) {
        // values_json 格式: {"temp":23.5,"humidity":65.2}
        const char* p = readings[i].values_json;
        char search_key[24];
        std::snprintf(search_key, sizeof(search_key), "\"%s\":", field);
        const char* found = std::strstr(p, search_key);
        if (found) {
            const char* num_start = found + std::strlen(search_key);
            float val = 0;
            if (std::sscanf(num_start, "%f", &val) == 1) {
                return val;
            }
        }
    }
    return -1e9f;
}

void Automation::StartUi(void* disp)
{
    lv_disp_ = disp;
    ESP_LOGI(TAG, "Automation UI bound to display");
}

// ==================== 运行时检查 ====================

void Automation::TaskMain(void* arg)
{
    auto* self = static_cast<Automation*>(arg);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kCheckIntervalMs));
        self->CheckAndExecute();
    }
}

void Automation::CheckAndExecute()
{
    if (!fan_ || !registry_) return;

    for (int i = 0; i < count_; ++i) {
        const Rule& rule = rules_[i];
        if (!rule.enabled) continue;

        float val = SensorValue(rule.sensor_field);
        if (val < -1e6f) continue;  // 该字段暂无数据

        bool triggered = rule_active_[i];

        if (!triggered) {
            // 当前未激活：检查是否应触发
            if (ShouldTrigger(rule, val)) {
                triggered = true;
                rule_active_[i] = true;
                last_trigger_us_[i] = esp_timer_get_time();
                last_trigger_val_[i] = val;
                ESP_LOGI(TAG, "rule[%d] TRIGGERED: %s=%.1f, fan -> %d%%",
                         i, rule.sensor_field, val, rule.fan_power);

                // 只有规则指定控制风扇时才下发
                if (rule.fan_power >= 0 && fan_) {
                    fan_->SetPowerByRule(rule.fan_power);
                }
            }
        } else {
            // 当前已激活：检查是否应保持运行
            uint64_t now_us = esp_timer_get_time();

            // 定时时长到期
            if (rule.duration_sec > 0) {
                uint64_t elapsed_us = now_us - last_trigger_us_[i];
                if (elapsed_us >= rule.duration_sec * 1000000ULL) {
                    ESP_LOGI(TAG, "rule[%d] duration expired, stopping fan", i);
                    rule_active_[i] = false;
                    if (rule.fan_power > 0 && fan_) {
                        fan_->SetPowerByRule(0);
                    }
                    continue;
                }
            }

            // 条件解除（带迟滞）
            if (!ShouldKeepRunning(rule, val, last_trigger_us_[i])) {
                ESP_LOGI(TAG, "rule[%d] condition cleared (val=%.1f vs thresh=%.1f), stopping fan",
                         i, val, rule.threshold);
                rule_active_[i] = false;
                if (rule.fan_power > 0 && fan_) {
                    fan_->SetPowerByRule(0);
                }
            }
        }
    }
}

// ==================== 条件判断（迟滞保护）====================

bool Automation::ShouldTrigger(const Rule& rule, float current_val)
{
    // 触发条件：实际阈值 + 迟滞（让触发更积极一点）
    float trigger_thresh = rule.threshold;

    switch (rule.op) {
    case RuleOp::GT:   return current_val > trigger_thresh;
    case RuleOp::LT:   return current_val < trigger_thresh;
    case RuleOp::GTE:  return current_val >= trigger_thresh;
    case RuleOp::LTE:  return current_val <= trigger_thresh;
    }
    return false;
}

bool Automation::ShouldKeepRunning(const Rule& rule, float current_val,
                                   uint64_t trigger_time_us)
{
    // 带迟滞的解除判断
    float clear_thresh = rule.threshold;
    if (rule.hysteresis > 0) {
        // > 规则解除: val < threshold - hysteresis
        // < 规则解除: val > threshold + hysteresis
        switch (rule.op) {
        case RuleOp::GT:
        case RuleOp::GTE:
            clear_thresh = rule.threshold - rule.hysteresis;
            return current_val > clear_thresh;  // 还高于解除阈值 → 继续
        case RuleOp::LT:
        case RuleOp::LTE:
            clear_thresh = rule.threshold + rule.hysteresis;
            return current_val < clear_thresh;  // 还低于解除阈值 → 继续
        }
    }

    // 无迟滞：原始条件取反
    switch (rule.op) {
    case RuleOp::GT:   return current_val > rule.threshold;
    case RuleOp::LT:   return current_val < rule.threshold;
    case RuleOp::GTE:  return current_val >= rule.threshold;
    case RuleOp::LTE:  return current_val <= rule.threshold;
    }
    return false;
}

} // namespace esp32node
