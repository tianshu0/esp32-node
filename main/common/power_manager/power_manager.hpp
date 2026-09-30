// power_manager 组件：低功耗管理（可选）
//
// 节点默认全速运行（app_config.power_save = false），因为 BLE 广播期间
// 进入浅睡会中断可发现性，不适合常驻。
//
// 电池供电场景可打开开关：启用 ESP-IDF 电源管理（DFS + 自动 light sleep），
// 由系统在空闲时自动降频/休眠；BLE 协议栈在连接/广播期间会自行持有电源锁，
// 因此不会影响配对与通信。
#pragma once

#include "esp_err.h"

namespace esp32node {

class AppConfig;

class PowerManager {
public:
    PowerManager() = default;
    ~PowerManager() = default;

    // 依据 app_config.power_save 决定是否启用自动 light sleep。
    // 配置失败只告警、不阻断启动（低功耗是可选能力）。
    esp_err_t Init(AppConfig* config);

    bool Enabled() const { return enabled_; }

private:
    bool enabled_ = false;
};

} // namespace esp32node
