#include "sensor_driver/SensorDriver.hpp"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "sensor_driver";

esp_err_t SensorDriver::Init(AppConfig* config, SensorRegistry* registry)
{
    if (config == nullptr || registry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = bus_.Init(config->I2cSda(), config->I2cScl());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // SHT3X：温度 + 相对湿度
    if (sht3x_.Init(bus_) == ESP_OK) {
        sht3x_ready_ = true;
        registry->Register("temp_hum", "SHT3X",
                           "{\"temp\":\"float\",\"humidity\":\"float\",\"unit\":\"C/%\"}",
                           &Sht3x::ReadThunk, &sht3x_);
    } else {
        ESP_LOGW(TAG, "sht3x init failed, check wiring (addr 0x44/0x45)");
    }

    // BMP180：气压 + 温度 + 海拔
    if (bmp180_.Init(bus_) == ESP_OK) {
        bmp180_.SetSeaLevelHpa(config->SeaLevelHpa());
        bmp180_ready_ = true;
        registry->Register("pressure", "BMP180",
                           "{\"pressure\":\"float\",\"temp\":\"float\","
                           "\"altitude\":\"float\",\"unit\":\"hPa/C/m\"}",
                           &Bmp180::ReadThunk, &bmp180_);
    } else {
        ESP_LOGW(TAG, "bmp180 init failed, check wiring (addr 0x77)");
    }

    ESP_LOGI(TAG, "sensor driver ready: %d sensor(s) registered", registry->Count());
    return ESP_OK;
}

} // namespace esp32node
