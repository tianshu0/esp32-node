#include "link/ble_link.hpp"

#if CONFIG_BT_ENABLED

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"

#include "nimble/nimble_port.h"
#include "esp_err.h"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "ble_link";

BleLink::BleLink(AppConfig& config, SensorRegistry& registry)
    : config_(&config), registry_(&registry)
{
}

esp_err_t BleLink::Start()
{
    // NimBLE host 必须先于 GATT 服务注册
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ble_.Init(config_, registry_);
}

const char* BleLink::StatusText() const
{
    if (ble_.IsPaired()) {
        return "PAIRED";
    }
    if (ble_.IsConnected()) {
        return "CONN";
    }
    return "ADV";
}

} // namespace esp32node

#endif // CONFIG_BT_ENABLED
