// ble：NimBLE Peripheral
//   （广播 + GATT 服务端 + 握手响应 + 周期采集 + 数据 Notify）
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
//   - 采集调度内聚在本组件（原独立 data_pipeline 组件已合并）：
//     采集节奏（配对开始 / 断开停止 / interval_ms）本质是 BLE 会话参数，
//     唯一数据出口也是本组件的 Notify，拆开只会多一条事件绕行路径
//   - 「采集任务打包 -> tx 队列 -> 发送任务 Notify」两段式：
//     发送放独立任务，避免在采集任务栈上做 NimBLE 调用
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

namespace esp32node {

class AppConfig;
class SensorRegistry;

class Ble {
public:
    Ble() = default;
    ~Ble();

    // 注册 GATT 服务 + 启动 NimBLE host 任务 / 采集任务 / 发送任务（任务在本函数内创建）
    esp_err_t Init(AppConfig* config, SensorRegistry* registry);

    // 供外部查询连接/配对状态
    bool IsConnected() const { return conn_handle_ != BLE_HS_CONN_HANDLE_NONE; }
    bool IsPaired() const { return paired_; }

private:
    // 一包上报数据：定长缓冲，跨队列传递（避免引用栈对象）
    struct SensorPacket {
        char json[192];
    };

    static void HostTask(void* arg);      // NimBLE host 事件循环任务入口
    static void CollectTask(void* arg);   // 采集任务：配对后按间隔采集打包入队
    static void TaskMain(void* arg);      // 数据发送任务
    static void OnReset(int reason);
    static void OnSync();
    static int GapEventCb(ble_gap_event* event, void* arg);
    static int GattAccessCb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt* ctxt, void* arg);

    // GATT 服务定义（函数内静态表：可访问私有静态回调）
    static const struct ble_gatt_svc_def* ServiceTable();

    void Advertise();
    void HandleCommand(const char* json, int len);
    void SendHelloAck();
    bool SendNotify(const char* data, int len);
    void RunCollect();
    void CollectOnce();
    void SetCollecting(bool paired);   // 握手完成/断开时切换采集状态

    static Ble* s_instance_;

    AppConfig* config_ = nullptr;
    SensorRegistry* registry_ = nullptr;

    TaskHandle_t task_ = nullptr;
    TaskHandle_t collect_task_ = nullptr;
    QueueHandle_t tx_queue_ = nullptr;
    EventGroupHandle_t collect_events_ = nullptr;

    uint8_t own_addr_type_ = 0;
    uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    uint16_t h_notify_ = 0;
    bool paired_ = false;

    static constexpr EventBits_t kPaired = 1 << 0;
    static constexpr EventBits_t kUnpaired = 1 << 1;
    static constexpr int kMaxReadings = 4;

    // 缓冲区做成成员：握手回调运行在 NimBLE host 任务栈上，避免大栈帧
    char rx_buf_[256];
    char tx_buf_[1024];
    char notify_cache_[384];
};

} // namespace esp32node
