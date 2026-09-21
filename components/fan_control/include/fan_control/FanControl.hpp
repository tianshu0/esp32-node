// FanControl：单路 PWM 风扇控制组件
//
// 使用 ESP32 LEDC 外设产生 PWM 信号，支持：
//   - 开/关（通过 duty=0 或最大）
//   - 无级调速（0-100% 或 0-255/1023/4095 分辨率）
//   - 手动 override（规则引擎自动控制 vs 用户手动控制）
//   - 状态查询（当前功率、是否被 override、运行时长）
//
// 4 线 PWM 风扇的工作原理：
//   - 红线 VCC (5V)、黑线 GND
//   - 黄线 TACH（转速反馈，可选接）
//   - 蓝线 PWM（1-25kHz PWM 输入，占空比决定转速）
//
// 本组件只用 PWM 控制，不读转速反馈。
#pragma once

#include "esp_err.h"
#include <cstdint>
#include <cstring>

namespace esp32node {

class FanControl {
public:
    FanControl() = default;
    ~FanControl() = default;

    // 初始化 LEDC 通道
    //   gpio:       PWM 输出 GPIO
    //   freq_hz:    PWM 频率（风扇通常 25kHz）
    //   resolution: duty 位宽（8=0-255, 10=0-1023, 12=0-4095）
    esp_err_t Init(int gpio, uint32_t freq_hz, uint32_t resolution);

    // 设置风扇功率（0-100%）
    //   pct:        0-100，设 0 表示关闭
    //   manual_override: true 时写入用户手动控制标记，自动规则引擎会跳过
    esp_err_t SetPower(int pct, bool manual_override = false);

    // 规则引擎调用的定时设置（不受手动 override 影响）
    esp_err_t SetPowerByRule(int pct);

    // 清除手动 override，恢复自动控制
    void ClearManualOverride();

    // 查询当前状态
    int CurrentPower()    const { return current_pct_; }
    bool IsRunning()      const { return current_pct_ > 0; }
    bool IsManual()       const { return manual_override_; }
    uint32_t RunTimeSec() const;  // 本次连续运行秒数（0=已关闭）

private:
    // 实际写 LEDC duty（SetPower/SetPowerByRule 共用）
    esp_err_t SetPowerInternal(int pct);

    int gpio_ = -1;
    uint32_t resolution_ = 8;
    uint32_t max_duty_ = 255;
    int current_pct_ = 0;
    bool manual_override_ = false;
    uint64_t last_start_us_ = 0;  // esp_timer_get_time() 记录
};

} // namespace esp32node
