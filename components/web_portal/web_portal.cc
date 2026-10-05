#include "web_portal.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <cJSON.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define TAG "WebPortal"

// 配网页面静态资源：源文件位于本组件 web/ 目录，由 CMakeLists 的
// EMBED_TXTFILES 嵌入固件（符号名 = 文件名中的 . 替换为 _，尾部自带 null）。
extern const char kIndexHtmlStart[] asm("_binary_index_html_start");
extern const char kIndexHtmlEnd[] asm("_binary_index_html_end");
extern const char kStyleCssStart[] asm("_binary_style_css_start");
extern const char kStyleCssEnd[] asm("_binary_style_css_end");
extern const char kAppJsStart[] asm("_binary_app_js_start");
extern const char kAppJsEnd[] asm("_binary_app_js_end");

// 统一发送嵌入式静态资源（no-store：门户地址固定，避免升级后浏览器用旧缓存）
static esp_err_t SendEmbeddedFile(httpd_req_t* req, const char* start,
                                  const char* end, const char* type) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start - 1);  // 去掉尾部 null
}

// ---------------- 生命周期 ----------------

WebPortal::~WebPortal() {
    Stop();
}

esp_err_t WebPortal::Start(CredentialsCallback on_credentials) {
    if (server_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    on_credentials_ = std::move(on_credentials);
    status_ = PortalStatus::kIdle;

    // 读取 SoftAP 网关 IP（DNS 劫持应答与跳转目标）
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != nullptr) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(ap_netif, &info) == ESP_OK) {
            ap_ip_ = info.ip.addr;
        }
    }
    if (ap_ip_ == 0) {
        ap_ip_ = ipaddr_addr("192.168.4.1");  // 兜底：SoftAP 默认网关
    }

    // DNS 劫持任务：所有域名解析到 AP 网关（captive portal 探测的前提）。
    // 失败不阻塞门户——用户仍可手动访问网关 IP。
    dns_running_ = true;
    BaseType_t ret = xTaskCreate(DnsTaskEntry, "portal_dns", 3072, this, 3, &dns_task_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create dns task");
        dns_running_ = false;
        dns_task_ = nullptr;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;   // /scan 含 cJSON 组包与扫描等待
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    // 默认配置为简单字符串匹配，"/*" 兜底 302 依赖通配符匹配
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        server_ = nullptr;
        return err;
    }

    const char* uris[] = {"/", "/style.css", "/app.js", "/scan", "/save", "/status"};
    esp_err_t (*handlers[])(httpd_req_t*) = {
        HandleIndex, HandleStyleCss, HandleAppJs,
        HandleScan, HandleSave, HandleStatus,
    };
    for (int i = 0; i < 6; i++) {
        httpd_uri_t uri = {};
        uri.uri = uris[i];
        uri.method = HTTP_GET;
        uri.handler = handlers[i];
        uri.user_ctx = this;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &uri));
    }
    // /save 走 POST
    httpd_uri_t save_uri = {};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = HandleSave;
    save_uri.user_ctx = this;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &save_uri));
    // 兜底：其余 GET 全部 302 回配置页（注册顺序在具体路径之后）
    httpd_uri_t redirect_uri = {};
    redirect_uri.uri = "/*";
    redirect_uri.method = HTTP_GET;
    redirect_uri.handler = HandleRedirect;
    redirect_uri.user_ctx = this;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &redirect_uri));

    ESP_LOGI(TAG, "portal started");
    return ESP_OK;
}

void WebPortal::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    if (dns_running_) {
        dns_running_ = false;
        // DNS 任务 1s 接收超时轮询退出标志
        for (int i = 0; i < 30 && dns_task_ != nullptr; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        dns_task_ = nullptr;
    }
    status_ = PortalStatus::kIdle;
}

void WebPortal::SetStatus(PortalStatus status) {
    status_ = status;
}

// ---------------- HTTP 处理器 ----------------

esp_err_t WebPortal::HandleIndex(httpd_req_t* req) {
    return SendEmbeddedFile(req, kIndexHtmlStart, kIndexHtmlEnd,
                            "text/html; charset=utf-8");
}

esp_err_t WebPortal::HandleStyleCss(httpd_req_t* req) {
    return SendEmbeddedFile(req, kStyleCssStart, kStyleCssEnd, "text/css");
}

esp_err_t WebPortal::HandleAppJs(httpd_req_t* req) {
    return SendEmbeddedFile(req, kAppJsStart, kAppJsEnd,
                            "application/javascript; charset=utf-8");
}

esp_err_t WebPortal::HandleScan(httpd_req_t* req) {
    auto* self = static_cast<WebPortal*>(req->user_ctx);
    (void)self;

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // 阻塞式，约 1~2s
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 15) {
        ap_count = 15;  // 页面最多展示 15 条（扫描结果默认按 RSSI 降序）
    }
    std::vector<wifi_ap_record_t> records(ap_count);
    if (ap_count > 0) {
        esp_wifi_scan_get_ap_records(&ap_count, records.data());
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* aps = cJSON_AddArrayToObject(root, "aps");
    for (uint16_t i = 0; i < ap_count; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;  // 跳过隐藏网络
        }
        cJSON* ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", reinterpret_cast<const char*>(records[i].ssid));
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(ap, "auth", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(aps, ap);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    cJSON_free(json);
    return ret;
}

esp_err_t WebPortal::HandleSave(httpd_req_t* req) {
    auto* self = static_cast<WebPortal*>(req->user_ctx);

    int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_OK;
    }
    std::vector<char> body(total + 1);
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, body.data() + received, total - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
            return ESP_OK;
        }
        received += n;
    }
    body[total] = '\0';

    cJSON* root = cJSON_Parse(body.data());
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON* pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    const char* ssid_str = cJSON_IsString(ssid) ? cJSON_GetStringValue(ssid) : "";
    const char* pass_str = cJSON_IsString(pass) ? cJSON_GetStringValue(pass) : "";
    bool valid = ssid_str[0] != '\0' && strlen(ssid_str) <= 32 && strlen(pass_str) <= 64;
    if (valid && self->on_credentials_) {
        self->on_credentials_(std::string(ssid_str), std::string(pass_str));
    }
    cJSON_Delete(root);

    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid/pass");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebPortal::HandleStatus(httpd_req_t* req) {
    auto* self = static_cast<WebPortal*>(req->user_ctx);
    const char* state = "idle";
    switch (self->status_.load()) {
        case PortalStatus::kConnecting: state = "connecting"; break;
        case PortalStatus::kSuccess:    state = "success";    break;
        case PortalStatus::kFailed:     state = "failed";     break;
        case PortalStatus::kIdle:       state = "idle";       break;
    }
    char json[32];
    snprintf(json, sizeof(json), "{\"state\":\"%s\"}", state);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebPortal::HandleRedirect(httpd_req_t* req) {
    auto* self = static_cast<WebPortal*>(req->user_ctx);
    // ap_ip_ 为网络字节序，内存字节序即 192.168.4.1 的点分顺序
    uint32_t ip = self->ap_ip_;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
    char url[32];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", b[0], b[1], b[2], b[3]);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ---------------- DNS 劫持 ----------------

void WebPortal::DnsTaskEntry(void* arg) {
    static_cast<WebPortal*>(arg)->DnsLoop();
}

void WebPortal::DnsLoop() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ESP_LOGE(TAG, "dns bind 53 failed");
        close(sock);
        dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 1s 接收超时：轮询退出标志
    timeval tv = {};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "dns hijack on :53");
    uint8_t buf[512];
    while (dns_running_) {
        sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&src), &src_len);
        if (len < 12 || (buf[2] & 0x80) != 0) {
            continue;  // 无效包或非查询
        }
        uint16_t qdcount = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
        if (qdcount != 1) {
            continue;
        }

        // 响应 = 原始查询 + 置位 QR/RA + ANCOUNT=1 + 单条 A 记录指向 AP 网关
        if (len + 16 > static_cast<int>(sizeof(buf))) {
            continue;
        }
        buf[2] |= 0x80;  // QR=1 响应
        buf[3] |= 0x80;  // RA=1
        buf[7] = 1;      // ANCOUNT=1
        static const uint8_t kAnswerFixed[12] = {
            0xC0, 0x0C,              // 名称压缩指针，指向问题段
            0x00, 0x01,              // Type: A
            0x00, 0x01,              // Class: IN
            0x00, 0x00, 0x00, 0x3C,  // TTL: 60s
            0x00, 0x04,              // RDLENGTH: 4
        };
        memcpy(buf + len, kAnswerFixed, sizeof(kAnswerFixed));
        memcpy(buf + len + 12, &ap_ip_, 4);  // RDATA: AP 网关 IP（网络字节序）
        sendto(sock, buf, len + 16, 0, reinterpret_cast<sockaddr*>(&src), src_len);
    }

    close(sock);
    ESP_LOGI(TAG, "dns task exit");
    dns_task_ = nullptr;
    vTaskDelete(nullptr);
}
