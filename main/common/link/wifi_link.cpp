#include "link/wifi_link.hpp"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"

namespace esp32node {

WifiLink::WifiLink(AppConfig& config, SensorRegistry& registry, const FanHooks& fan)
    : config_(&config), registry_(&registry), fan_(fan)
{
}

esp_err_t WifiLink::Start()
{
    return wifi_.Init(config_, registry_, fan_);
}

const char* WifiLink::StatusText() const
{
    return wifi_.IsStaConnected() ? "已连接" : "未连接";
}

} // namespace esp32node
