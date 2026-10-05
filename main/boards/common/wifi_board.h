#pragma once

#include "board.h"

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "web_portal.h"

// WiFi 板基类：联网状态机 + 配网门户调度。
//
// 使用方式：派生板在构造函数末尾（显示屏就绪后）调用 InitializeNetwork()。
// 状态机在独立任务中运行：
//   1. NVS 无凭据 -> 直接进入配网模式（SoftAP + 网页门户 + 屏幕配网页）
//   2. 有凭据 -> STA 连接，60s 预算内反复重试；超时进入配网模式
//   3. 配网模式中每 30s 后台静默重试已保存凭据（路由器重启自愈）
//   4. 运行期掉线同样执行 60s 重试，超时退回配网模式
//   5. RequestProvisioning() 供派生板接入手动重配网入口（如屏幕长按手势）
class WifiBoard : public Board {
public:
    // 派生板构造函数末尾调用：初始化 WiFi 协议栈并启动状态机任务
    void InitializeNetwork();

    // 请求进入配网模式（已在配网模式时为空操作）
    void RequestProvisioning();

protected:
    WifiBoard() = default;

private:
    // 事件位（wifi_events_）
    static constexpr EventBits_t kConnectedBit = BIT0;     // IP_EVENT_STA_GOT_IP
    static constexpr EventBits_t kDisconnectedBit = BIT1;  // WIFI_EVENT_STA_DISCONNECTED
    static constexpr EventBits_t kSubmitBit = BIT2;        // 网页提交了新凭据

    // 策略参数
    static constexpr int kConnectTimeoutMs = 60000;        // STA 连接预算：60s
    static constexpr int kVerifyTimeoutMs = 20000;         // 配网页提交凭据的验证预算
    static constexpr int kRetryIntervalMs = 30000;         // 配网模式下后台重试间隔
    static constexpr int kRetryTimeoutMs = 12000;          // 后台重试单次预算

    static void WifiTask(void* arg);
    static void WifiEventHandler(void* arg, esp_event_base_t base,
                                 int32_t event_id, void* event_data);

    bool LoadCredentials(std::string& ssid, std::string& password);
    void SaveCredentials(const std::string& ssid, const std::string& password);
    // 带预算的 STA 连接：预算内每 6s 重新发起一次连接，拿到 IP 返回 true
    bool ConnectToAp(const std::string& ssid, const std::string& password, int timeout_ms);
    void EnterProvisioning();
    void ExitProvisioning();
    void ShowStatus(const char* status);

    WebPortal portal_;
    EventGroupHandle_t wifi_events_ = nullptr;
    bool wifi_started_ = false;

    // 配网状态（wifi_task 写；EnterProvisioning/ExitProvisioning 仅在 wifi_task 调用）
    std::atomic<bool> provisioning_{false};
    std::atomic<bool> provision_request_{false};

    // 网页提交的待验证凭据（httpd 线程 <-> wifi_task）
    std::mutex pending_mutex_;
    std::string pending_ssid_;
    std::string pending_password_;

    std::string ap_ssid_;  // SoftAP 名称：Node-XXXXXX（STA MAC 后 3 字节）
};
