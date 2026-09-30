#include "application.hpp"

#include "link/link.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

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

    ESP_RETURN_ON_ERROR(registry_.Init(), TAG, "sensor registry init failed");

    // 板级装配：创建总线、勾选的传感器、选中的显示屏与链路（屏/链路为可选项）
    board_ctx_.config = &config_;
    board_ctx_.registry = &registry_;
    board_ = &GetBoard(board_ctx_);
    board_->Assemble();

    // 链路启动：BLE 还是 WiFi 由板装配层决定（BleLink 内部先 nimble_port_init
    // 再注册 GATT 服务），Application 不感知传输类型
    Link* link = board_->GetLink();
    if (link == nullptr) {
        ESP_LOGE(TAG, "board %s provided no link", board_->Name());
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(link->Start(), TAG, "link start failed");

    power_.Init(&config_);

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

    // 读取所有传感器（与链路采集任务的并发访问由传感器驱动内部互斥保证）
    SensorReading samples[SensorRegistry::kMaxSensors] = {};
    int n = registry_.ReadAll(samples, SensorRegistry::kMaxSensors);
    disp->UpdateSamples(samples, n);

    // 链路状态由板选定的 Link 提供（BLE: ADV/CONN/PAIRED；WiFi: 已连接/未连接）
    disp->SetStatus(board_->GetLink()->StatusText());
}

} // namespace esp32node
