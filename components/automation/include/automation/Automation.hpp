// Automation：自动化规则引擎
//
// 功能：
//   1. 定期（每 5 秒）检查所有规则：传感器字段值 vs 阈值 → 条件成立则触发动作
//   2. 迟滞保护（hysteresis）：防止在阈值附近反复开关
//   3. 定时执行（duration_sec）：触发后运行指定时长，超时后自动停止
//   4. NVS 持久化：最多保存 8 条规则，掉电不丢失
//   5. LVGL UI 绑定：允许从屏幕浏览/增删改规则
//   6. 支持手动 override：FanControl.IsManual() 时规则自动跳过
//
// 规则数据结构：
//   sensor_field: 哪个传感器字段触发（"temperature" / "humidity" / "tvoc" / ...）
//   op:           比较运算符（> / < / >= / <=）
//   threshold:    阈值
//   fan_power:    触发后风扇功率 (0-100, 0 表示不控制风扇)
//   duration_sec: 自动关闭时长（0 = 持续到条件解除）
//   hysteresis:   迟滞值（0 = 无）
//   enabled:      是否启用
#pragma once

#include "esp_err.h"
#include "fan_control/fan_control.hpp"
#include "sensor_registry/sensor_registry.hpp"
#include "app_config/app_config.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <cstring>

namespace esp32node {

// 最多保存的规则数量
static constexpr int kMaxRules = 8;

// 比较运算符
enum class RuleOp : uint8_t {
    GT = 0,  // >
    LT = 1,  // <
    GTE = 2, // >=
    LTE = 3, // <=
};

// 规则结构（直接用 memcpy 存 NVS，确保字段对齐）
struct Rule {
    char sensor_field[16];   // 传感器字段 key
    RuleOp op;               // 比较运算符
    float threshold;         // 阈值
    int8_t fan_power;        // 风扇功率 0-100（-1 表示不控制风扇）
    uint32_t duration_sec;   // 运行时长 (0 = 持续到条件解除)
    float hysteresis;        // 迟滞值 (0 = 无)
    bool enabled;            // 是否启用
    bool _padding[2];        // 对齐
};

// 布局：16(field) + 1(op) + 3(pad) + 4(threshold) + 1(fan_power) + 3(pad)
//      + 4(duration_sec) + 4(hysteresis) + 1(enabled) + 2(_padding) + 1(pad) = 40
static_assert(sizeof(Rule) == 40, "Rule struct size");

class Automation {
public:
    Automation() = default;
    ~Automation() = default;

    esp_err_t Init(AppConfig* config, SensorRegistry* registry, FanControl* fan);

    // 从 NVS 加载规则（Init 内部调用，也可手动重新加载）
    esp_err_t LoadRules();

    // 保存单条规则到 NVS（rules[idx] 覆写并立即持久化）
    esp_err_t SaveRule(int idx, const Rule& rule);

    // 删除（禁用）规则 idx
    esp_err_t DisableRule(int idx);

    // 读取规则
    int  RuleCount()         const { return count_; }
    const Rule& GetRule(int idx) const { return rules_[idx]; }

    // 供 UI 查询某个传感器字段的当前值（从 registry 最新采集）
    float SensorValue(const char* field) const;

    // 绑定 LVGL 显示对象（用 void* 避免本组件强制依赖 LVGL；
    // 实际传入 lv_disp_t*，由后续规则编辑页使用）
    void StartUi(void* disp);

private:
    // 每 5 秒的规则检查回调
    static void TaskMain(void* arg);
    void CheckAndExecute();

    // 单条规则的触发条件判断（带迟滞）
    bool ShouldTrigger(const Rule& rule, float current_val);
    bool ShouldKeepRunning(const Rule& rule, float current_val,
                           uint64_t trigger_time_us);

    AppConfig*       config_   = nullptr;
    SensorRegistry*  registry_ = nullptr;
    FanControl*      fan_      = nullptr;
    void*            lv_disp_  = nullptr;  // 实际类型 lv_disp_t*
    Rule rules_[kMaxRules] = {};
    int  count_ = 0;

    // 运行时状态：每条规则的最近触发时间和触发值
    uint64_t last_trigger_us_[kMaxRules] = {};  // esp_timer_get_time()
    float    last_trigger_val_[kMaxRules] = {};
    bool     rule_active_[kMaxRules] = {};       // 当前正在控制风扇

    TaskHandle_t task_ = nullptr;
};

} // namespace esp32node
