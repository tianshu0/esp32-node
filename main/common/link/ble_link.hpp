// BleLink：BLE 链路（节点作为 NimBLE 外设，广播 + 握手 + 周期上报）
//
// 持有并驱动 ble 组件，对 Application 只暴露 Link 接口。
// 仅 CONFIG_BT_ENABLED 的项目（C3 节点）有实现。
#pragma once

#if CONFIG_BT_ENABLED

#include "link/link.hpp"
#include "ble/ble.hpp"

namespace esp32node {

class AppConfig;
class SensorRegistry;

class BleLink : public Link {
public:
    BleLink(AppConfig& config, SensorRegistry& registry);

    esp_err_t Start() override;
    const char* StatusText() const override;

private:
    AppConfig* config_ = nullptr;
    SensorRegistry* registry_ = nullptr;
    Ble ble_;
};

} // namespace esp32node

#endif // CONFIG_BT_ENABLED
