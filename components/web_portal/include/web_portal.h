#pragma once

#include <atomic>
#include <functional>
#include <string>

#include <esp_err.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 配网验证状态：由调用方（WifiBoard）回填，网页通过 GET /status 轮询读取
enum class PortalStatus {
    kIdle,        // 空闲（初始）
    kConnecting,  // 正在验证凭据
    kSuccess,     // 凭据有效，已联网
    kFailed,      // 凭据无效，等待重试
};

// 纯 Web 配网门户组件：DNS 劫持(UDP 53) + HTTP 服务(TCP 80) + 内嵌中文配置页。
// 不依赖 main/；SoftAP 由调用方（WifiBoard）负责拉起，本组件只管端口与页面。
//
// 页面 API：
//   GET  /           配置页（HTML，web/index.html）
//   GET  /style.css  样式表（web/style.css）
//   GET  /app.js     页面脚本（web/app.js）
//   GET  /scan       周边 AP 列表（JSON，内部走 esp_wifi 扫描）
//   POST /save       提交 {ssid, pass}，触发凭据回调
//   GET  /status     配网验证状态（JSON）
//   GET  /*          其余路径 302 跳转到配置页（captive portal 探测兜底）
class WebPortal {
public:
    // 用户在网页提交凭据后回调（httpd 工作线程上下文，调用方自行线程安全处理）
    using CredentialsCallback =
        std::function<void(const std::string& ssid, const std::string& password)>;

    WebPortal() = default;
    ~WebPortal();

    WebPortal(const WebPortal&) = delete;
    WebPortal& operator=(const WebPortal&) = delete;

    // 启动 DNS 劫持与 HTTP 服务；需 SoftAP 已启动并拿到网关 IP
    esp_err_t Start(CredentialsCallback on_credentials);

    // 停止服务并释放端口
    void Stop();

    // 回填配网验证状态
    void SetStatus(PortalStatus status);

private:
    static void DnsTaskEntry(void* arg);
    void DnsLoop();

    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleStyleCss(httpd_req_t* req);
    static esp_err_t HandleAppJs(httpd_req_t* req);
    static esp_err_t HandleScan(httpd_req_t* req);
    static esp_err_t HandleSave(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);
    static esp_err_t HandleRedirect(httpd_req_t* req);

    httpd_handle_t server_ = nullptr;
    CredentialsCallback on_credentials_;
    TaskHandle_t dns_task_ = nullptr;
    std::atomic<bool> dns_running_{false};
    std::atomic<PortalStatus> status_{PortalStatus::kIdle};

    // AP 网关 IP（DNS 劫持应答与 302 跳转目标）
    uint32_t ap_ip_ = 0;  // 网络字节序
};
