#include "application.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"

#if CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp32node {

static const char* TAG = "esp32-node";

esp_err_t Application::Init()
{
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    // NVS 必须先于 Wi-Fi 初始化：phy 校准数据的读写要走 NVS，否则会报
    // "esp_phy_load_cal_data_from_nvs: NVS has not been initialized" 并退回全量校准。
    ESP_RETURN_ON_ERROR(config_.Init(), TAG, "app config init failed");

#if CONFIG_BT_ENABLED
    // NimBLE 协议栈初始化（host 必须先于 ble_peripheral 启动）
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");
#endif

    ESP_RETURN_ON_ERROR(registry_.Init(), TAG, "sensor registry init failed");

#if CONFIG_BT_ENABLED
    ESP_RETURN_ON_ERROR(ble_.Init(&config_, &registry_), TAG, "ble init failed");
#endif

    // 板级装配：创建总线、勾选的传感器、选中的显示屏（屏为可选项，失败只告警）
    board_ctx_.config = &config_;
    board_ctx_.registry = &registry_;
#if CONFIG_BT_ENABLED
    board_ctx_.ble = &ble_;
#endif
    board_ = &GetBoard(board_ctx_);
    board_->Assemble();

    power_.Init(&config_);

    // portal 依赖装配输出（fan 指针），必须在 Assemble 之后
    ESP_RETURN_ON_ERROR(portal_.Init(&config_, board_->GetFan(), &registry_),
                        TAG, "wifi portal init failed");

    // 1s 周期采集任务：读取传感器并推给显示
    BaseType_t ok = xTaskCreate(SensorTask, "sensor", 3072, this, 4, &sensor_task_);
    if (ok != pdPASS) {
        sensor_task_ = nullptr;
        ESP_LOGE(TAG, "xTaskCreate sensor task failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void Application::Run()
{
    ESP_LOGI(TAG, "esp32-node started: board=%s id=%s i2c(sda=%d scl=%d) sensors=%d display=%s",
             board_->Name(),
             config_.NodeId().c_str(), config_.I2cSda(), config_.I2cScl(),
             registry_.Count(), board_->DisplayPresent() ? "on" : "off");
}

// ==================== 1s 采集任务 ====================

void Application::SensorTask(void* arg)
{
    auto* app = static_cast<Application*>(arg);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        app->UpdateDisplay();
    }
}

void Application::UpdateDisplay()
{
    Display* disp = board_->GetDisplay();
    if (!disp) {
        return;
    }

    // 读取所有传感器（与 BLE 采集任务的并发访问由传感器驱动内部互斥保证）
    SensorReading samples[SensorRegistry::kMaxSensors] = {};
    int n = registry_.ReadAll(samples, SensorRegistry::kMaxSensors);
    disp->UpdateSamples(samples, n);

    // 推送 BLE 连接状态（C3 OLED 板）或 WiFi 状态（S3 TFT 板）
#if CONFIG_BT_ENABLED
    const char* status = "ADV";
    if (ble_.IsPaired()) {
        status = "PAIRED";
    } else if (ble_.IsConnected()) {
        status = "CONN";
    }
    disp->SetStatus(status);
#else
    // 无 BLE 项目（如 s3_tft_fan）：显示 WiFi 连接状态
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = false;
    if (sta) {
        esp_netif_ip_info_t info = {};
        connected = (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0);
    }
    disp->SetStatus(connected ? "已连接" : "未连接");
#endif
}

} // namespace esp32node
