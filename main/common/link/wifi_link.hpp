// WifiLink：WiFi 链路（SoftAP 配网 + STA 上报）
//
// 持有并驱动 wifi 组件，对 Application 只暴露 Link 接口。
// 风扇控制以 FanHooks 回调注入（依赖反转），wifi 组件不再直接依赖 FanControl。
#pragma once

#include "link/link.hpp"
#include "wifi/wifi.hpp"

namespace esp32node {

class AppConfig;
class SensorRegistry;

class WifiLink : public Link {
public:
    // fan 可为空 hooks（无风扇板型，网页隐藏风扇开关）
    WifiLink(AppConfig& config, SensorRegistry& registry, const FanHooks& fan = FanHooks{});

    esp_err_t Start() override;
    const char* StatusText() const override;

private:
    AppConfig* config_ = nullptr;
    SensorRegistry* registry_ = nullptr;
    FanHooks fan_;
    Wifi wifi_;
};

} // namespace esp32node
