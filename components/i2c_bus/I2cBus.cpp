#include "i2c_bus/I2cBus.hpp"

#include "esp_log.h"
#include "esp_system.h"

namespace esp32node {

static const char* TAG = "i2c_bus";

I2cBus::~I2cBus()
{
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    if (bus_ != nullptr) {
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
    }
}

esp_err_t I2cBus::Init(int sda_gpio, int scl_gpio, uint32_t clk_hz)
{
    if (bus_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "create bus mutex failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = static_cast<gpio_num_t>(sda_gpio);
    cfg.scl_io_num = static_cast<gpio_num_t>(scl_gpio);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    // 模块板载已有上拉，这里再开内部上拉以提高容错
    cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&cfg, &bus_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
        bus_ = nullptr;
        return err;
    }
    clk_hz_ = clk_hz;
    ESP_LOGI(TAG, "i2c bus ready: sda=%d scl=%d clk=%luHz",
             sda_gpio, scl_gpio, static_cast<unsigned long>(clk_hz_));
    return ESP_OK;
}

esp_err_t I2cBus::AddDevice(uint8_t addr_7bit, i2c_master_dev_handle_t* out)
{
    if (bus_ == nullptr || out == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = addr_7bit;
    dev.scl_speed_hz = clk_hz_;

    esp_err_t err = i2c_master_bus_add_device(bus_, &dev, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", addr_7bit, esp_err_to_name(err));
    }
    return err;
}

bool I2cBus::Probe(uint8_t addr_7bit)
{
    return bus_ != nullptr && i2c_master_probe(bus_, addr_7bit, kProbeTimeoutMs) == ESP_OK;
}

void I2cBus::Lock()
{
    if (mutex_ != nullptr) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
}

void I2cBus::Unlock()
{
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }
}

} // namespace esp32node
