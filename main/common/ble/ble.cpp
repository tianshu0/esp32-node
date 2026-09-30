// Ble 实现：NimBLE Peripheral 角色
//
// 状态机：
//   上电 -> NimBLE sync -> 广播（含 ff00 服务 UUID）
//        -> hub 连接 -> hub 订阅 ff02 CCC + 写 hello
//        -> 回 hello_ack（node_id + 能力清单）
//        -> 收 accept（interval_ms）-> 回 ack_done -> 启动周期采集
//        -> 周期性 Notify 传感器数据
//   断开 -> 暂停采集 -> 重新广播，等 hub 重连
//
// 为避免循环依赖，ble 不引入 sensors 驱动：
//   - 能力清单来自 sensor_registry（只有描述，没有驱动细节）
//   - 采集调度内聚在本组件（原 data_pipeline 组件已合并，见头文件说明）
#include "ble/ble.hpp"
#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"

#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace esp32node {

static const char* TAG = "ble";

// 自定义服务/特征值 UUID（与 esp32-hub 协议一致）
// BLE_UUID128_INIT 的字节序为「小端」：value[0] 是 UUID 字符串的最后一个字节。
//   服务：  0000ff00-0000-1000-8000-00805f9b34fb
//   WRITE： 0000ff01-0000-1000-8000-00805f9b34fb
//   NOTIFY：0000ff02-0000-1000-8000-00805f9b34fb
static const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00);
static const ble_uuid128_t kChrWriteUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00);
static const ble_uuid128_t kChrNotifyUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00);

// 协议栈注册后回填 ff02 的值句柄（CCCD 为其 +1，与 hub 端假设一致）
static uint16_t s_notify_val_handle = 0;

static constexpr const char* kDeviceName = "esp32-node";

Ble* Ble::s_instance_ = nullptr;

// ================ GATT 服务定义 ================

const struct ble_gatt_svc_def* Ble::ServiceTable()
{
    static const struct ble_gatt_chr_def chars[] = {
        {
            // ff01：hub -> node 握手请求（Write Request / Write Command）
            .uuid = &kChrWriteUuid.u,
            .access_cb = &Ble::GattAccessCb,
            .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        },
        {
            // ff02：node -> hub 响应/数据（Notify，同时可读便于调试）
            // 未声明 CCC 描述符：协议栈会按 NOTIFY 属性自动追加到值句柄之后
            .uuid = &kChrNotifyUuid.u,
            .access_cb = &Ble::GattAccessCb,
            .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
            .val_handle = &s_notify_val_handle,
        },
        {}   // 特征值表结束
    };

    static const struct ble_gatt_svc_def svcs[] = {
        {
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &kSvcUuid.u,
            .characteristics = chars,
        },
        {}   // 服务表结束
    };
    return svcs;
}

// ================ 生命周期 ================

Ble::~Ble()
{
    if (collect_events_ != nullptr) {
        vEventGroupDelete(collect_events_);
    }
    if (tx_queue_ != nullptr) {
        vQueueDelete(tx_queue_);
    }
    if (s_instance_ == this) {
        s_instance_ = nullptr;
    }
}

esp_err_t Ble::Init(AppConfig* config, SensorRegistry* registry)
{
    if (config == nullptr || registry == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    registry_ = registry;
    s_instance_ = this;

    tx_queue_ = xQueueCreate(4, sizeof(SensorPacket));
    if (tx_queue_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    collect_events_ = xEventGroupCreate();
    if (collect_events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // 注册 GAP/GATT 基础服务 + 自定义 ff00 服务
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(ServiceTable());
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(ServiceTable());
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }
    // 注意：此刻 GATT 尚未 start，s_notify_val_handle 还是 0，不能在这里取。
    // 句柄要等 OnSync（ble_gatts_start 完成）后才有效，订阅事件里再兜底一次。

    rc = ble_svc_gap_device_name_set(kDeviceName);
    if (rc != 0) {
        ESP_LOGW(TAG, "set device name failed: %d", rc);
    }

    // NimBLE host 配置与回调
    ble_hs_cfg.reset_cb = &Ble::OnReset;
    ble_hs_cfg.sync_cb = &Ble::OnSync;

    // 启动 NimBLE host 事件循环（host 任务在本函数内创建）
    nimble_port_freertos_init(&Ble::HostTask);

    // 数据发送任务：从队列取包 -> GATT Notify
    BaseType_t tx_task_ok = xTaskCreate(&Ble::TaskMain, "ble_tx", 4096,
                                        this, 6, &task_);
    if (tx_task_ok != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "xTaskCreate ble_tx failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_ERR_NO_MEM;
    }

    // 采集任务：配对后按 interval 采集打包入队（原 data_pipeline 职责）
    BaseType_t collect_ok = xTaskCreate(&Ble::CollectTask, "ble_collect", 4096,
                                        this, 5, &collect_task_);
    if (collect_ok != pdPASS) {
        collect_task_ = nullptr;
        ESP_LOGE(TAG, "xTaskCreate ble_collect failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ble peripheral initialized (node_id=%s, notify handle assigned after sync)",
             config->NodeId().c_str());
    return ESP_OK;
}

void Ble::HostTask(void* /*arg*/)
{
    // 事件循环返回即协议栈停止，届时释放 host
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void Ble::OnReset(int reason)
{
    ESP_LOGE(TAG, "nimble host reset, reason=%d", reason);
}

void Ble::OnSync()
{
    if (s_instance_ == nullptr) {
        return;
    }

    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_instance_->own_addr_type_);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    // host sync 时 ble_gatts_start 已执行，ff02 值句柄已分配，此刻取才有效
    s_instance_->h_notify_ = s_notify_val_handle;
    ESP_LOGI(TAG, "gatt ready, ff02 notify handle=%d", s_instance_->h_notify_);

    s_instance_->Advertise();
}

// ================ 广播 ================

void Ble::Advertise()
{
    if (ble_gap_adv_active()) {
        return;
    }

    // 广播包：flags + 128 位服务 UUID（hub 靠 AD type 0x06/0x07 匹配设备）
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kSvcUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    // 设备名放扫描响应，避免广播包超过 31 字节
    struct ble_hs_adv_fields rsp = {};
    const char* name = ble_svc_gap_device_name();
    rsp.name = reinterpret_cast<const uint8_t*>(name);
    rsp.name_len = static_cast<uint8_t>(std::strlen(name));
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv rsp set fields failed: %d", rc);
    }

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params,
                           &Ble::GapEventCb, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\" (service ff00)", name);
}

// ================ GAP 事件回调 ================

int Ble::GapEventCb(ble_gap_event* event, void* arg)
{
    Ble* self = static_cast<Ble*>(arg);
    if (self == nullptr) {
        self = s_instance_;
    }
    if (self == nullptr || event == nullptr) {
        return 0;
    }

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            self->conn_handle_ = event->connect.conn_handle;
            self->paired_ = false;
            ESP_LOGI(TAG, "hub connected, handle=%d", self->conn_handle_);
        } else {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            self->Advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected, reason=%d", event->disconnect.reason);
        self->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        self->paired_ = false;
        // 暂停采集，然后重新广播等 hub 重连
        self->SetCollecting(false);
        self->Advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete, reason=%d", event->adv_complete.reason);
        self->Advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated: %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: conn=%d attr=%d notify=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        // 兜底：以协议栈在订阅事件中给出的 ff02 值句柄为准（正常应与 sync 时取到的一致）
        if (event->subscribe.cur_notify && event->subscribe.attr_handle != 0) {
            self->h_notify_ = event->subscribe.attr_handle;
        }
        return 0;

    default:
        return 0;
    }
}

// ================ GATT 读写回调 ================

int Ble::GattAccessCb(uint16_t /*conn_handle*/, uint16_t /*attr_handle*/,
                                struct ble_gatt_access_ctxt* ctxt, void* arg)
{
    Ble* self = static_cast<Ble*>(arg);
    if (self == nullptr) {
        self = s_instance_;
    }
    if (self == nullptr || ctxt == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len >= sizeof(self->rx_buf_)) {
            ESP_LOGW(TAG, "write too long: %u", static_cast<unsigned>(len));
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, self->rx_buf_, sizeof(self->rx_buf_) - 1, &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        self->rx_buf_[len] = '\0';
        ESP_LOGI(TAG, "rx: %s", self->rx_buf_);
        self->HandleCommand(self->rx_buf_, len);
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:
        // ff02 可读：返回最近一次 Notify 内容（便于调试）
        if (self->notify_cache_[0] != '\0') {
            int rc = os_mbuf_append(ctxt->om, self->notify_cache_,
                                    static_cast<uint16_t>(std::strlen(self->notify_cache_)));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

// ================ 握手 ================

void Ble::HandleCommand(const char* json, int len)
{
    if (json == nullptr || len <= 0) {
        return;
    }

    cJSON* root = cJSON_ParseWithLength(json, static_cast<size_t>(len));
    if (root == nullptr) {
        ESP_LOGW(TAG, "handshake json parse failed");
        return;
    }
    const cJSON* act = cJSON_GetObjectItem(root, "act");
    if (!cJSON_IsString(act) || act->valuestring == nullptr) {
        cJSON_Delete(root);
        return;
    }

    if (std::strcmp(act->valuestring, "hello") == 0) {
        // 第 1 步：hub 握手请求 -> 记录 relay_id -> 上报能力清单
        const cJSON* relay = cJSON_GetObjectItem(root, "relay_id");
        if (cJSON_IsString(relay) && relay->valuestring[0] != '\0') {
            config_->SetHubId(relay->valuestring);
            ESP_LOGI(TAG, "hello from %s", relay->valuestring);
        }
        SendHelloAck();
    } else if (std::strcmp(act->valuestring, "accept") == 0) {
        // 第 3 步：hub 接受配对 -> 保存上报间隔 -> 确认 -> 通知开始采集
        const cJSON* interval = cJSON_GetObjectItem(root, "interval_ms");
        if (cJSON_IsNumber(interval) && interval->valueint > 0) {
            config_->SetReportIntervalMs(static_cast<uint32_t>(interval->valueint));
        }
        ESP_LOGI(TAG, "paired with %s, interval=%lu ms",
                 config_->HubId().c_str(),
                 static_cast<unsigned long>(config_->ReportIntervalMs()));

        static constexpr char kAckDone[] = "{\"act\":\"ack_done\"}";
        SendNotify(kAckDone, static_cast<int>(sizeof(kAckDone) - 1));

        paired_ = true;
        SetCollecting(true);
    } else {
        ESP_LOGW(TAG, "unknown act: %s", act->valuestring);
    }

    cJSON_Delete(root);
}

void Ble::SendHelloAck()
{
    const int written = std::snprintf(
        tx_buf_, sizeof(tx_buf_),
        "{\"act\":\"hello_ack\",\"node_id\":\"%s\",\"ver\":1,"
        "\"types\":%s,\"sensors\":%s}",
        config_->NodeId().c_str(), registry_->TypesJson(), registry_->DetailJson());
    if (written <= 0 || written >= static_cast<int>(sizeof(tx_buf_))) {
        ESP_LOGE(TAG, "hello_ack too long, drop");
        return;
    }
    SendNotify(tx_buf_, written);
}

// ================ Notify 发送 ================

bool Ble::SendNotify(const char* data, int len)
{
    if (data == nullptr || len <= 0) {
        return false;
    }
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE || h_notify_ == 0) {
        // 句柄未就绪（未连接/未订阅，或 sync 前取值为 0），显式打日志避免静默丢包
        ESP_LOGW(TAG, "notify skipped: conn=%d notify_handle=%d",
                 conn_handle_, h_notify_);
        return false;
    }

    // ATT Notification 单包载荷上限 = 协商 MTU - 3（1 字节 opcode + 2 字节句柄），
    // 超长时 NimBLE 不报错而是静默截断（hello_ack 曾被切到 253 字节导致 hub 解析出
    // 空能力清单）。发送前显式拦截，让协议超长作为可见错误暴露。
    const int mtu_payload = static_cast<int>(ble_att_mtu(conn_handle_)) - 3;
    if (len > mtu_payload) {
        ESP_LOGE(TAG, "notify too long: len=%d > mtu payload=%d (mtu=%d)",
                 len, mtu_payload, ble_att_mtu(conn_handle_));
        return false;
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
    if (om == nullptr) {
        ESP_LOGW(TAG, "notify mbuf alloc failed");
        return false;
    }
    // ble_gatts_notify_custom 内部会释放 om（无论成功与否）
    int rc = ble_gatts_notify_custom(conn_handle_, h_notify_, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: rc=%d", rc);
        return false;
    }

    const int copy = (len < static_cast<int>(sizeof(notify_cache_)) - 1)
                         ? len
                         : static_cast<int>(sizeof(notify_cache_)) - 1;
    std::memcpy(notify_cache_, data, static_cast<size_t>(copy));
    notify_cache_[copy] = '\0';
    return true;
}

// ================ 数据发送任务 ================

void Ble::TaskMain(void* arg)
{
    Ble* self = static_cast<Ble*>(arg);
    SensorPacket packet = {};

    for (;;) {
        if (xQueueReceive(self->tx_queue_, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!self->paired_ || self->conn_handle_ == BLE_HS_CONN_HANDLE_NONE) {
            continue;   // 握手未完成/已断开，丢弃
        }
        self->SendNotify(packet.json, static_cast<int>(std::strlen(packet.json)));
    }
}

// ================ 采集（原 data_pipeline 职责，组件内直连发送队列） ================

void Ble::SetCollecting(bool paired)
{
    if (collect_events_ == nullptr) {
        return;
    }
    if (paired) {
        xEventGroupClearBits(collect_events_, kUnpaired);
        xEventGroupSetBits(collect_events_, kPaired);
    } else {
        xEventGroupClearBits(collect_events_, kPaired);
        xEventGroupSetBits(collect_events_, kUnpaired);
    }
}

void Ble::CollectTask(void* arg)
{
    Ble* self = static_cast<Ble*>(arg);
    self->RunCollect();
    vTaskDelete(nullptr);
}

void Ble::RunCollect()
{
    ESP_LOGI(TAG, "collect task started, waiting for pairing");

    for (;;) {
        // 等握手完成
        xEventGroupWaitBits(collect_events_, kPaired, pdFALSE, pdTRUE, portMAX_DELAY);

        const uint32_t interval_ms = config_->ReportIntervalMs();
        ESP_LOGI(TAG, "paired, sampling every %lu ms", static_cast<unsigned long>(interval_ms));

        while (xEventGroupGetBits(collect_events_) & kPaired) {
            CollectOnce();

            // 睡到下一个采集周期；期间若断开则 kUnpaired 置位，立即退出
            EventBits_t bits = xEventGroupWaitBits(
                collect_events_, kUnpaired, pdTRUE, pdFALSE, pdMS_TO_TICKS(interval_ms));
            if (bits & kUnpaired) {
                break;
            }
        }
        ESP_LOGW(TAG, "unpaired, sampling paused");
    }
}

void Ble::CollectOnce()
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

        if (xQueueSend(tx_queue_, &packet, 0) != pdTRUE) {
            ESP_LOGW(TAG, "tx queue full, drop %s", readings[i].type);
        }
    }
}

} // namespace esp32node
