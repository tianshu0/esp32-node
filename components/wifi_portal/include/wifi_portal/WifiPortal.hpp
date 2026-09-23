// wifi_portal：手机连接 SoftAP 后打开网页进行配网与控制
//
// 功能：
//   1. SoftAP（开放网络，SSID 形如 FanNode-XXXX）+ DNS 抢答实现 Captive Portal，
//      手机连上后自动弹出配置页；未弹出可手动访问 http://192.168.4.1
//   2. 网页：设备状态（节点ID / 湿度 / 风扇 / WiFi）+ 风扇开关 + WiFi 扫描与配网
//   3. 配网凭据经 AppConfig 写入 NVS，开机时若有保存凭据则 AP+STA 自动尝试连接
//      （连接失败自动重试，AP 配网入口始终可用）
#pragma once

#include "esp_err.h"

namespace esp32node {

class AppConfig;
class FanControl;
class SensorRegistry;

class WifiPortal {
public:
    // 默认事件循环必须已创建（main.cpp 已建）。
    // fan / registry 可为 nullptr：无风扇板型网页隐藏风扇开关，无传感器时湿度显示 --
    esp_err_t Init(AppConfig* config, FanControl* fan, SensorRegistry* registry);
};

} // namespace esp32node
