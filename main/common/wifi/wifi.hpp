// wifi：手机连接 SoftAP 后打开网页进行配网与控制（Captive Portal）
//
// 功能：
//   1. SoftAP（开放网络，SSID 形如 FanNode-XXXX）+ DNS 抢答实现 Captive Portal，
//      手机连上后自动弹出配置页；未弹出可手动访问 http://192.168.4.1
//   2. 网页：设备状态（节点ID / 湿度 / 风扇 / WiFi）+ 风扇开关 + WiFi 扫描与配网
//   3. 配网凭据经 AppConfig 写入 NVS，开机时若有保存凭据则 AP+STA 自动尝试连接
//      （连接失败自动重试，AP 配网入口始终可用）
#pragma once

#include <functional>

#include "esp_err.h"

namespace esp32node {

class AppConfig;
class SensorRegistry;

// 风扇控制回调（依赖反转）：由板装配层绑定具体 FanControl 对象，
// wifi 组件不再 include / 依赖 fan_control；无风扇板型传空 hooks。
struct FanHooks {
    std::function<bool()>    is_running;  // 当前是否运行
    std::function<int()>     power;       // 当前功率 0-100
    std::function<void(int)> set_power;   // 设置功率（用户手动，覆盖规则引擎）

    bool valid() const { return is_running && power && set_power; }
};

class Wifi {
public:
    // 默认事件循环必须已创建（Application 已建）。
    // registry 可为 nullptr：无传感器时网页湿度显示 --；
    // fan.valid() == false 时网页隐藏风扇开关。
    esp_err_t Init(AppConfig* config, SensorRegistry* registry,
                   const FanHooks& fan = FanHooks{});

    // STA 是否已连上 AP 并拿到 IP（供 Link 查询连接状态）
    bool IsStaConnected() const;
};

} // namespace esp32node
