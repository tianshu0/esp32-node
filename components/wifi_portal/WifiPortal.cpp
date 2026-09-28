#include "wifi_portal/WifiPortal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "fan_control/FanControl.hpp"
#include "freertos/event_groups.h"

namespace esp32node {

static const char* TAG = "wifi_portal";
static constexpr const char* kPortalUrl = "http://192.168.4.1/";

// STA 连接状态机事件位（参考 esp32-hub WifiManager）
static constexpr EventBits_t kStaStarted = 1 << 0;
static constexpr EventBits_t kGotIp      = 1 << 1;
static constexpr EventBits_t kDisconnect = 1 << 2;

// 单实例：WiFi 事件回调与 HTTP handler 都是无 context 的 C 回调，直接用文件级指针
static WifiPortal* s_self = nullptr;
static AppConfig* s_config = nullptr;
static FanControl* s_fan = nullptr;
static SensorRegistry* s_registry = nullptr;
static volatile bool s_sta_got_ip = false;      // STA 是否已拿到 IP
static char s_sta_ip[16] = "";                  // STA IP 字符串
static EventGroupHandle_t s_wifi_events = nullptr;
static int s_sta_retry = 0;                     // STA 断线重连计数

// ==================== 配置页 ====================

// 页面是独立文件 web/index.html，由 CMake EMBED_FILES 在编译期嵌入固件只读段
// （无需 SPIFFS 分区，单 bin 烧录）。符号名只取文件 basename（web/ 目录前缀
// 不进入符号名）；数据不以 '\0' 结尾，长度须用 end - start 计算。
extern const char kIndexHtmlStart[] asm("_binary_index_html_start");
extern const char kIndexHtmlEnd[]   asm("_binary_index_html_end");

// ==================== 工具函数 ====================

// 从注册表读数 JSON 里抠浮点值（与 main/display 相同的轻量解析）
static bool ExtractJsonFloat(const char* json, const char* key, float* out)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    char* end = nullptr;
    float v = strtof(p + strlen(pattern), &end);
    if (end == p + strlen(pattern)) return false;
    *out = v;
    return true;
}

// JSON 字符串转义（SSID 可能含引号/控制字符）
static std::string JsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) >= 0x20) { o += c; }
    }
    return o;
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string UrlDecode(const char* begin, const char* end)
{
    std::string o;
    o.reserve(end - begin);
    for (const char* s = begin; s < end; ++s) {
        if (*s == '%' && s + 2 < end && HexVal(s[1]) >= 0 && HexVal(s[2]) >= 0) {
            o += static_cast<char>((HexVal(s[1]) << 4) | HexVal(s[2]));
            s += 2;
        } else if (*s == '+') {
            o += ' ';
        } else {
            o += *s;
        }
    }
    return o;
}

// 解析 form-urlencoded："ssid=xxx&pass=yyy"
static bool ParseForm(const char* body, std::string* ssid, std::string* pass)
{
    const char* p = strstr(body, "ssid=");
    if (!p) return false;
    p += 5;
    const char* amp = strchr(p, '&');
    *ssid = UrlDecode(p, amp ? amp : p + strlen(p));
    if (amp) {
        const char* q = strstr(amp + 1, "pass=");
        if (q) *pass = UrlDecode(q + 5, q + 5 + strlen(q + 5));
    }
    return true;
}

// ==================== SNTP 网络校时 ====================

// 时区：中国标准时间 UTC+8（POSIX TZ 字符串 "CST-8"）
static void SntpSyncCb(struct timeval* tv)
{
    time_t now = tv->tv_sec;
    struct tm t = {};
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "sntp synced: %04d-%02d-%02d %02d:%02d:%02d (CST)",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

static void InitSntp()
{
    if (esp_sntp_enabled()) return;  // 已初始化过则只重新同步
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(SntpSyncCb);
    esp_sntp_init();
    ESP_LOGI(TAG, "sntp started (tz=CST-8)");
}

// ==================== WiFi 事件 ====================

static void OnWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_events, kStaStarted);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_got_ip = false;
        xEventGroupSetBits(s_wifi_events, kDisconnect);
        if (s_sta_retry < 5) {
            ++s_sta_retry;
            ESP_LOGW(TAG, "sta disconnected, retry %d/5", s_sta_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "sta connect failed, portal still at %s", kPortalUrl);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_got_ip = true;
        s_sta_retry = 0;
        xEventGroupSetBits(s_wifi_events, kGotIp);
        ESP_LOGI(TAG, "sta connected: %s", s_sta_ip);
        InitSntp();  // 联网后启动 NTP 校时，状态栏显示真实时间
    }
}

// 连接 STA：设配置 → 清状态位 → connect → 等 GOT_IP（超时 15s）
// 参考 esp32-hub WifiManager::TryConnectStation + WaitGotIp
static bool ConnectStation(const std::string& ssid, const std::string& pass)
{
    if (ssid.empty()) return false;

    wifi_config_t sta_cfg = {};
    strncpy(reinterpret_cast<char*>(sta_cfg.sta.ssid), ssid.c_str(),
            sizeof(sta_cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(sta_cfg.sta.password), pass.c_str(),
            sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) return false;

    // 清位再 connect，避免事件早于清除而丢失
    xEventGroupClearBits(s_wifi_events, kStaStarted | kGotIp | kDisconnect);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, kGotIp | kDisconnect,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & kGotIp) != 0;
}

// ==================== HTTP handlers ====================

// 未知路径（含各手机系统的联网探测 URL）一律 302 回配置页 → Captive Portal 生效
static esp_err_t ErrorRedirect(httpd_req_t* req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", kPortalUrl);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t HandlerIndex(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kIndexHtmlStart,
                    static_cast<ssize_t>(kIndexHtmlEnd - kIndexHtmlStart));
    return ESP_OK;
}

// 浏览器自动请求 favicon.ico，返回空 204 避免 404 日志刷屏
static esp_err_t HandlerFavicon(httpd_req_t* req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// 通配符兜底：手机系统的 captive portal 探测（/generate_204、/connecttest.txt 等）
// 一律返回配置页，确保弹出配网界面，同时抑制 "URI not found" 404 日志
static esp_err_t HandlerCatchAll(httpd_req_t* req)
{
    return HandlerIndex(req);
}

static esp_err_t HandlerStatus(httpd_req_t* req)
{
    std::string out = "{\"node_id\":\"" + JsonEscape(s_config->NodeId()) + "\"";

    float humi = 0;
    bool have = false;
    if (s_registry) {
        SensorReading readings[4];
        int n = s_registry->ReadAll(readings, 4);
        for (int i = 0; i < n && !have; ++i) {
            have = ExtractJsonFloat(readings[i].values_json, "humidity", &humi);
        }
    }
    if (have) {
        char b[16];
        snprintf(b, sizeof(b), "%.1f", humi);
        out += ",\"humi\":" + std::string(b);
    } else {
        out += ",\"humi\":null";
    }

    if (s_fan) {
        out += std::string(",\"fan\":") + (s_fan->IsRunning() ? "true" : "false");
        char b[12];
        snprintf(b, sizeof(b), "%d", s_fan->CurrentPower());
        out += ",\"power\":" + std::string(b);
    } else {
        out += ",\"fan\":false,\"power\":0";
    }

    std::string w;
    if (s_sta_got_ip) {
        w = std::string("已连接 ") + s_sta_ip;
    } else if (!s_config->WifiSsid().empty()) {
        w = "连接中…";
    } else {
        w = "未配置";
    }
    out += ",\"sta\":\"" + JsonEscape(w) + "\"";
    out += ",\"set_ssid\":\"" + JsonEscape(s_config->WifiSsid()) + "\"";
    out += "}";

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t HandlerScan(httpd_req_t* req)
{
    // 全信道扫描以列出所有附近 AP。
    // 代价：扫描期间射频会离开 AP 信道，SoftAP beacon 短暂中断，手机可能显示
    // "无互联网"但通常不会彻底断开（扫描总时长 < 1.5s）。若用户反馈断连，
    // 可把 max 调到 60ms 以下或回退到仅扫 AP 信道。
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    sc.channel = 0;                            // 0 = 全信道（1~14）
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    // 本设备未启用 BLE，可安全使用自定义扫描时间缩短全信道扫描总耗时
    sc.scan_time.active.min = 0;              // 0 = 用默认最短
    sc.scan_time.active.max = 100;            // 每信道最多 100ms，14 信道 ≈ 1.4s

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_OK;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 15) n = 15;
    std::vector<wifi_ap_record_t> recs(n ? n : 1);
    if (n > 0) {
        esp_wifi_scan_get_ap_records(&n, recs.data());
    }

    std::string out = "{\"aps\":[";
    bool first = true;
    for (int i = 0; i < static_cast<int>(n); ++i) {
        const char* ssid = reinterpret_cast<const char*>(recs[i].ssid);
        if (!ssid[0]) continue;  // 隐藏网络跳过
        if (!first) out += ',';
        first = false;
        out += "{\"s\":\"" + JsonEscape(ssid) + "\",\"r\":";
        out += std::to_string(recs[i].rssi) + "}";
    }
    out += "]}";

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t HandlerSaveWifi(httpd_req_t* req)
{
    char body[256] = {};
    int total = 0, ret;
    while (total < static_cast<int>(sizeof(body)) - 1 &&
           (ret = httpd_req_recv(req, body + total, sizeof(body) - 1 - total)) > 0) {
        total += ret;
    }

    std::string ssid, pass;
    if (!ParseForm(body, &ssid, &pass) || ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }

    esp_err_t err = s_config->SetWifi(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid/pass length");
        return ESP_OK;
    }

    // 立即尝试连接（AP 保持开启，连不上也不影响配网入口）
    s_sta_retry = 0;
    bool ok = ConnectStation(ssid, pass);

    std::string msg = ok
        ? ("已保存，连接成功 " + std::string(s_sta_ip))
        : ("已保存，正在连接 " + ssid + " … 成功后本页 WiFi 栏会显示 IP");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, msg.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t HandlerFanToggle(httpd_req_t* req)
{
    if (!s_fan) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no fan on this board");
        return ESP_OK;
    }
    if (s_fan->IsRunning()) {
        s_fan->SetPower(0, true);
    } else {
        s_fan->SetPower(100, true);
    }
    char b[48];
    snprintf(b, sizeof(b), "{\"fan\":%s,\"power\":%d}",
             s_fan->IsRunning() ? "true" : "false", s_fan->CurrentPower());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ==================== 迷你 DNS 抢答器（Captive Portal 核心） ====================
// IDF 5.4 无 dns_server 组件，自己实现：UDP 53 收到任意 A 查询都答 192.168.4.1，
// AAAA 答空记录（避免 IPv6 探测超时），其他类型忽略。

// DNS 报文偏移
static constexpr size_t kDnsOffQdcount = 4;   // QDCOUNT
static constexpr size_t kDnsOffQuestion = 12; // Question 区起始
static const uint8_t kApIp[4] = {192, 168, 4, 1};  // AP 网关 IP

static void DnsTask(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind :53 failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        uint8_t req[512];
        sockaddr_in from = {};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, req, sizeof(req), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < (int)(kDnsOffQuestion + 5)) continue;

        uint16_t qdcount = (req[kDnsOffQdcount] << 8) | req[kDnsOffQdcount + 1];
        if (qdcount != 1) continue;

        // Question 区：QNAME（以 0 结尾）+ QTYPE(2) + QCLASS(2)
        const uint8_t* q = req + kDnsOffQuestion;
        while (q < req + n && *q) ++q;
        ++q;                                   // 跳过结尾 0
        if (q + 4 > req + n) continue;
        uint16_t qtype = (q[0] << 8) | q[1];
        size_t qlen = (q + 4) - (req + kDnsOffQuestion);

        // 仅响应 A(1) / AAAA(28) / ANY(255)
        if (qtype != 1 && qtype != 28 && qtype != 255) continue;

        uint8_t resp[512];
        size_t i = 0;
        resp[i++] = req[0]; resp[i++] = req[1];              // ID 原样
        resp[i++] = 0x81; resp[i++] = 0x80;                  // 响应 + 递归可用
        resp[i++] = 0; resp[i++] = 1;                        // QDCOUNT=1
        resp[i++] = 0; resp[i++] = (qtype == 28) ? 0 : 1;    // ANCOUNT（AAAA 答空）
        resp[i++] = 0; resp[i++] = 0;                        // NSCOUNT
        resp[i++] = 0; resp[i++] = 0;                        // ARCOUNT
        memcpy(resp + i, req + kDnsOffQuestion, qlen);       // 原样回 Question
        i += qlen;

        if (qtype != 28) {                                   // A 记录答案
            resp[i++] = 0xC0; resp[i++] = 12;                // NAME 指针 → Question
            resp[i++] = 0; resp[i++] = 1;                    // TYPE=A
            resp[i++] = 0; resp[i++] = 1;                    // CLASS=IN
            resp[i++] = 0; resp[i++] = 0; resp[i++] = 0; resp[i++] = 60;  // TTL
            resp[i++] = 0; resp[i++] = 4;                    // RDLENGTH=4
            memcpy(resp + i, kApIp, 4);                      // RDATA: 192.168.4.1
            i += 4;
        }

        sendto(sock, resp, i, 0, reinterpret_cast<sockaddr*>(&from), fromlen);
    }
}

// ==================== 启动 ====================

esp_err_t WifiPortal::Init(AppConfig* config, FanControl* fan, SensorRegistry* registry)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_self) {
        ESP_LOGW(TAG, "portal already started");
        return ESP_OK;
    }
    s_self = this;
    s_config = config;
    s_fan = fan;
    s_registry = registry;

    // 默认事件循环 main 已创建；这里只补 netif 与 WiFi 事件注册
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    s_wifi_events = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiEvent, nullptr, nullptr);

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init failed");

    // SSID：FanNode-XXXX（MAC 末 2 字节）
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "FanNode-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {};
    strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;  // 开放网络：手机连上即可弹配网页

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "ap cfg failed");
    esp_wifi_set_ps(WIFI_PS_NONE);  // 关闭省电，提升 AP 连接稳定性
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    // 已保存过配网：开机即尝试连接（断线事件里自动重试 5 次）
    if (!config->WifiSsid().empty()) {
        ESP_LOGI(TAG, "sta connecting saved ssid: %s", config->WifiSsid().c_str());
        ConnectStation(config->WifiSsid(), config->WifiPass());
    }

    // HTTP 服务器
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size = 6144;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;  // 支持 /* 通配符
    httpd_handle_t server = nullptr;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &http_cfg), TAG, "httpd start failed");

    const httpd_uri_t uris[] = {
        { "/",            HTTP_GET,  &HandlerIndex,     nullptr },
        { "/status",      HTTP_GET,  &HandlerStatus,    nullptr },
        { "/scan",        HTTP_GET,  &HandlerScan,      nullptr },
        { "/wifi",        HTTP_POST, &HandlerSaveWifi,  nullptr },
        { "/fan",         HTTP_POST, &HandlerFanToggle, nullptr },
        { "/favicon.ico", HTTP_GET,  &HandlerFavicon,   nullptr },
        { "/*",           HTTP_GET,  &HandlerCatchAll,  nullptr },
    };
    for (const auto& u : uris) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG,
                            "register %s failed", u.uri);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, &ErrorRedirect);

    // DNS 抢答任务：所有域名都解析到本机 → 手机连上自动弹配置页
    if (xTaskCreate(DnsTask, "dns_portal", 3072, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "dns task create failed (portal 需手动访问 %s)", kPortalUrl);
    }

    ESP_LOGI(TAG, "portal ready: AP=%s page=%s", ssid, kPortalUrl);
    return ESP_OK;
}

} // namespace esp32node
