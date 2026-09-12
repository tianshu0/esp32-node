#include "data_pipeline/DataPipeline.hpp"

#include <cstdio>
#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "data_pipeline";

const char* kNodeEventBase = "node_pipeline";

DataPipeline::~DataPipeline()
{
    if (handler_registered_) {
        esp_event_handler_unregister(kNodeEventBase, ESP_EVENT_ANY_ID,
                                     &DataPipeline::EventHandler);
    }
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
    }
}

esp_err_t DataPipeline::Init(AppConfig* config, SensorRegistry* registry)
{
    if (config == nullptr || registry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    registry_ = registry;

    events_ = xEventGroupCreate();
    if (events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_register(kNodeEventBase, ESP_EVENT_ANY_ID,
                                               &DataPipeline::EventHandler, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register event handler failed: %s", esp_err_to_name(err));
        return err;
    }
    handler_registered_ = true;

    // 启动自身任务（组件自包含生命周期）
    xTaskCreate(&DataPipeline::TaskMain, "data_pipeline", 4096, this, 5, &task_);
    ESP_LOGI(TAG, "data pipeline ready (event base: %s)", kNodeEventBase);
    return ESP_OK;
}

void DataPipeline::TaskMain(void* arg)
{
    DataPipeline* self = static_cast<DataPipeline*>(arg);
    self->Run();
    vTaskDelete(nullptr);
}

void DataPipeline::EventHandler(void* arg, esp_event_base_t base, int32_t id, void* /*data*/)
{
    DataPipeline* self = static_cast<DataPipeline*>(arg);
    if (base != kNodeEventBase || self == nullptr || self->events_ == nullptr) {
        return;
    }
    switch (id) {
    case kEventPaired:
        xEventGroupClearBits(self->events_, kUnpaired);
        xEventGroupSetBits(self->events_, kPaired);
        break;
    case kEventUnpaired:
        xEventGroupClearBits(self->events_, kPaired);
        xEventGroupSetBits(self->events_, kUnpaired);
        break;
    default:
        break;
    }
}

void DataPipeline::Run()
{
    ESP_LOGI(TAG, "pipeline task started, waiting for pairing");

    for (;;) {
        // 等握手完成
        xEventGroupWaitBits(events_, kPaired, pdFALSE, pdTRUE, portMAX_DELAY);

        const uint32_t interval_ms = config_->ReportIntervalMs();
        ESP_LOGI(TAG, "paired, sampling every %lu ms", static_cast<unsigned long>(interval_ms));

        while (xEventGroupGetBits(events_) & kPaired) {
            CollectOnce();

            // 睡到下一个采集周期；期间若断开则 kUnpaired 置位，立即退出
            EventBits_t bits = xEventGroupWaitBits(
                events_, kUnpaired, pdTRUE, pdFALSE, pdMS_TO_TICKS(interval_ms));
            if (bits & kUnpaired) {
                break;
            }
        }
        ESP_LOGW(TAG, "unpaired, sampling paused");
    }
}

void DataPipeline::CollectOnce()
{
    SensorReading readings[kMaxReadings] = {};
    const int n = registry_->ReadAll(readings, kMaxReadings);
    if (n <= 0) {
        ESP_LOGW(TAG, "no valid sensor reading this cycle");
        return;
    }

    for (int i = 0; i < n; ++i) {
        SensorPacket packet = {};
        const int written = std::snprintf(
            packet.json, sizeof(packet.json),
            "{\"type\":\"%s\",\"ts\":%lld,\"values\":%s}",
            readings[i].type,
            static_cast<long long>(readings[i].ts_ms),
            readings[i].values_json);
        if (written <= 0 || written >= static_cast<int>(sizeof(packet.json))) {
            ESP_LOGW(TAG, "packet truncated, drop %s", readings[i].type);
            continue;
        }

        esp_err_t err = esp_event_post(kNodeEventBase, kEventSample,
                                       &packet, sizeof(packet), 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "post sample failed: %s", esp_err_to_name(err));
        }
    }
}

} // namespace esp32node
