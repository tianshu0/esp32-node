// ble_peripheral 组件：NimBLE Peripheral
//   （广播 + GATT 服务端 + 握手响应 + 数据 Notify）
//
// 协议（与 esp32-hub 的 ble_central 严格对应）：
//   - 服务 UUID：        0000ff00-0000-1000-8000-00805f9b34fb
//   - 特征值 WRITE：      0000ff01-...  hub→node 握手请求
//   - 特征值 NOTIFY/READ：0000ff02-...  node→hub 响应/数据
//   - CCC 描述符：        00002902-...
//     由协议栈在 ff02 带 NOTIFY 属性时自动挂到「值句柄 + 1」，
//     因此 hub 端「写 h_notify+1 订阅」的假设成立
//
// 握手 4 步（JSON over GATT）：
//   1. hub → node (WRITE)  {"act":"hello","relay_id":"hub-1A2B","ver":1}
//   2. node→ hub (NOTIFY)  {"act":"hello_ack","node_id":"node-xx","ver":1,
//                           "types":[...],"sensors":[...]}
//   3. hub → node (WRITE)  {"act":"accept","interval_ms":5000}
//   4. node→ hub (NOTIFY)  {"act":"ack_done"}
// 之后按 interval_ms 周期性 NOTIFY：
//   {"type":"temp_hum","ts":1234567890,"values":{...}}
//
// 设计要点：
//   - 广播包里携带 128 位服务 UUID（hub 用 AD type 0x06/0x07 匹配），
//     设备名放扫描响应，避免广播包超过 31 字节
//   - 断线后立即重新广播，等 hub 重连并重新握手
//   - 采样数据由 data_pipeline 投递事件，本组件转发 Notify；
//     发送放在独立任务里做，避免占用事件循环任务栈
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "esp_event.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "data_pipeline/DataPipeline.hpp"

namespace esp32node {

class AppConfig;
class SensorRegistry;

class BlePeripheral {
public:
    BlePeripheral() = default;
    ~BlePeripheral();

    // 注册 GATT 服务 + 启动 NimBLE host 任务与发送任务（任务在本函数内创建）
    esp_err_t Init(AppConfig* config, SensorRegistry* registry);

    // 供外部查询连接/配对状态
    bool IsConnected() const { return conn_handle_ != BLE_HS_CONN_HANDLE_NONE; }
    bool IsPaired() const { return paired_; }

private:
    static void HostTask(void* arg);   // NimBLE host 事件循环任务入口
    static void TaskMain(void* arg);   // 数据发送任务
    static void OnReset(int reason);
    static void OnSync();
    static int GapEventCb(ble_gap_event* event, void* arg);
    static int GattAccessCb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt* ctxt, void* arg);
    static void EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    // GATT 服务定义（函数内静态表：可访问私有静态回调）
    static const struct ble_gatt_svc_def* ServiceTable();

    void Advertise();
    void HandleCommand(const char* json, int len);
    void SendHelloAck();
    bool SendNotify(const char* data, int len);

    static BlePeripheral* s_instance_;

    AppConfig* config_ = nullptr;
    SensorRegistry* registry_ = nullptr;

    TaskHandle_t task_ = nullptr;
    QueueHandle_t tx_queue_ = nullptr;
    bool handler_registered_ = false;

    uint8_t own_addr_type_ = 0;
    uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    uint16_t h_notify_ = 0;
    bool paired_ = false;

    // 缓冲区做成成员：握手回调运行在 NimBLE host 任务栈上，避免大栈帧
    char rx_buf_[256];
    char tx_buf_[1024];
    char notify_cache_[384];
};

} // namespace esp32node
