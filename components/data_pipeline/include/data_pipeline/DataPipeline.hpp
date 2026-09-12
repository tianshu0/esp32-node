// data_pipeline 组件：定时采集 -> 打包 -> 事件投递
//
// 数据流：
//   BLE 握手完成 -> ble_peripheral 投递 kEventPaired
//     -> 本组件按 report_interval_ms 定时遍历 sensor_registry 采集
//     -> 每个传感器打包成 {"type":..,"ts":..,"values":{..}}
//     -> esp_event_post kEventSample
//   ble_peripheral 订阅 kEventSample -> GATT Notify 上报给 hub
//
// 设计要点：
//   - 事件基由本组件持有（对应 hub 端 sensor_pipeline 的角色）：
//     ble_peripheral 依赖本组件的事件定义，反向不依赖
//   - 本组件不知道 BLE 存在，只负责产生数据
//   - 配对前不采集，断线后自动暂停（由事件通知）
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

namespace esp32node {

class AppConfig;
class SensorRegistry;

// 自定义事件基（节点内部事件，与 ESP_EVENT_ANY_BASE 区分）
extern const char* kNodeEventBase;

class DataPipeline {
public:
    // 事件 ID
    enum EventId : int32_t {
        kEventSample = 1,     // data_pipeline -> ble_peripheral：一包传感器数据
        kEventPaired = 2,     // ble_peripheral -> data_pipeline：握手完成，开始采集
        kEventUnpaired = 3,   // ble_peripheral -> data_pipeline：连接断开，暂停采集
    };

    // 数据包：定长缓冲，避免 esp_event_post 引用栈对象
    struct SensorPacket {
        char json[192];
    };

    DataPipeline() = default;
    ~DataPipeline();

    // 注册事件基 + 启动采集任务（任务在本函数内 xTaskCreate）
    esp_err_t Init(AppConfig* config, SensorRegistry* registry);

private:
    static void TaskMain(void* arg);
    static void EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    void Run();
    void CollectOnce();

    AppConfig* config_ = nullptr;
    SensorRegistry* registry_ = nullptr;
    TaskHandle_t task_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    bool handler_registered_ = false;

    static constexpr EventBits_t kPaired = 1 << 0;
    static constexpr EventBits_t kUnpaired = 1 << 1;
    static constexpr int kMaxReadings = 4;
};

} // namespace esp32node
