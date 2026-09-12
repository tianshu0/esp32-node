#include "power_manager/PowerManager.hpp"

#include "app_config/AppConfig.hpp"
#include "esp_log.h"
#include "esp_pm.h"

namespace esp32node {

static const char* TAG = "power_manager";

esp_err_t PowerManager::Init(AppConfig* config)
{
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->PowerSave()) {
        ESP_LOGI(TAG, "power save disabled (enable via app_config.power_save)");
        return ESP_OK;
    }

    esp_pm_config_t pm = {};
    pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm.min_freq_mhz = 40;   // ESP32-C3 XTAL 40MHz，DFS 最低档
    pm.light_sleep_enable = true;

    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s (CONFIG_PM_ENABLE / "
                      "CONFIG_FREERTOS_USE_TICKLESS_IDLE 需打开)",
                 esp_err_to_name(err));
        return err;
    }

    enabled_ = true;
    ESP_LOGI(TAG, "power save enabled: DFS 40-%d MHz + automatic light sleep",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    return ESP_OK;
}

} // namespace esp32node
