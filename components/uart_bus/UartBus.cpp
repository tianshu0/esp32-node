#include "uart_bus/UartBus.hpp"

#include "esp_log.h"

namespace esp32node {

static const char* TAG = "uart_bus";

UartBus::~UartBus()
{
    if (installed_) {
        uart_driver_delete(port_);
        installed_ = false;
    }
}

esp_err_t UartBus::Init(uart_port_t port, int tx_gpio, int rx_gpio,
                        int baud, int rx_buf_size, int tx_buf_size)
{
    if (installed_) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = baud;
    cfg.data_bits = static_cast<uart_word_length_t>(kDefaultDataBits);
    cfg.parity = static_cast<uart_parity_t>(kDefaultParity);
    cfg.stop_bits = static_cast<uart_stop_bits_t>(kDefaultStopBits);
    cfg.flow_ctrl = static_cast<uart_hw_flowcontrol_t>(kDefaultFlowCtrl);
    cfg.rx_flow_ctrl_thresh = 0;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(port, rx_buf_size, tx_buf_size, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install(port=%d) failed: %s",
                 static_cast<int>(port), esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(port);
        return err;
    }

    err = uart_set_pin(port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin(tx=%d rx=%d) failed: %s",
                 tx_gpio, rx_gpio, esp_err_to_name(err));
        uart_driver_delete(port);
        return err;
    }

    port_ = port;
    installed_ = true;
    ESP_LOGI(TAG, "uart%d ready: tx=%d rx=%d baud=%d",
             static_cast<int>(port), tx_gpio, rx_gpio, baud);
    return ESP_OK;
}

int UartBus::Read(uint8_t* buf, size_t len, uint32_t timeout_ms)
{
    if (!installed_ || buf == nullptr || len == 0) {
        return 0;
    }
    return uart_read_bytes(port_, buf, len, pdMS_TO_TICKS(timeout_ms));
}

int UartBus::Write(const uint8_t* buf, size_t len)
{
    if (!installed_ || buf == nullptr || len == 0) {
        return 0;
    }
    return uart_write_bytes(port_, buf, len);
}

} // namespace esp32node
