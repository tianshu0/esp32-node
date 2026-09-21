// FanControl 实现：LEDC PWM + 手动 override + 运行时长追踪
#include "fan_control/FanControl.hpp"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <algorithm>

namespace esp32node {

static const char* TAG = "fan";

// LEDC 通道选择：通道 0，低速模式（LEDC_LOW_SPEED_MODE = 通用）
static constexpr ledc_channel_t kLedcChannel = LEDC_CHANNEL_0;
static constexpr ledc_mode_t    kLedcMode    = LEDC_LOW_SPEED_MODE;

esp_err_t FanControl::Init(int gpio, uint32_t freq_hz, uint32_t resolution)
{
    if (gpio < 0) {
        ESP_LOGE(TAG, "invalid gpio %d", gpio);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_ = gpio;
    resolution_ = resolution;
    max_duty_ = (1U << resolution) - 1;

    // 配置 LEDC 定时器
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = kLedcMode;
    timer_cfg.timer_num  = LEDC_TIMER_0;
    timer_cfg.duty_resolution = static_cast<ledc_timer_bit_t>(resolution);
    timer_cfg.freq_hz    = freq_hz;
    timer_cfg.clk_cfg    = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 配置 LEDC 通道
    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num    = gpio;
    ch_cfg.speed_mode  = kLedcMode;
    ch_cfg.channel     = kLedcChannel;
    ch_cfg.timer_sel   = LEDC_TIMER_0;
    ch_cfg.intr_type   = LEDC_INTR_DISABLE;
    ch_cfg.duty        = 0;  // 初始化时关闭
    ch_cfg.hpoint      = 0;

    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "PWM fan init: gpio=%d freq=%luHz res=%lu-bit max_duty=%lu",
             gpio, freq_hz, resolution, max_duty_);
    return ESP_OK;
}

esp_err_t FanControl::SetPower(int pct, bool manual_override)
{
    pct = std::clamp(pct, 0, 100);
    manual_override_ = manual_override;
    return SetPowerInternal(pct);
}

esp_err_t FanControl::SetPowerByRule(int pct)
{
    if (manual_override_) {
        ESP_LOGI(TAG, "fan manual override active, skip rule");
        return ESP_OK;  // 手动控制时跳过规则
    }
    pct = std::clamp(pct, 0, 100);
    return SetPowerInternal(pct);
}

void FanControl::ClearManualOverride()
{
    manual_override_ = false;
    ESP_LOGI(TAG, "fan manual override cleared, rule control resumed");
}

uint32_t FanControl::RunTimeSec() const
{
    if (current_pct_ <= 0 || last_start_us_ == 0) return 0;
    return static_cast<uint32_t>((esp_timer_get_time() - last_start_us_) / 1000000);
}

// ==================== 私有 ====================

esp_err_t FanControl::SetPowerInternal(int pct)
{
    if (pct == 0) {
        ESP_RETURN_ON_ERROR(ledc_set_duty(kLedcMode, kLedcChannel, 0),
                            TAG, "ledc_set_duty(0) failed");
    } else {
        uint32_t duty = static_cast<uint32_t>((max_duty_ * pct) / 100);
        ESP_RETURN_ON_ERROR(ledc_set_duty(kLedcMode, kLedcChannel, duty),
                            TAG, "ledc_set_duty(%lu) failed", duty);
    }
    ESP_RETURN_ON_ERROR(ledc_update_duty(kLedcMode, kLedcChannel),
                        TAG, "ledc_update_duty failed");

    bool was_off = (current_pct_ == 0);
    current_pct_ = pct;

    if (pct > 0 && was_off) {
        last_start_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "fan ON: power=%d%%", pct);
    } else if (pct == 0) {
        ESP_LOGI(TAG, "fan OFF");
        last_start_us_ = 0;
    }

    return ESP_OK;
}

} // namespace esp32node
