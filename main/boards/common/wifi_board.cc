#include "wifi_board.h"

#include "display.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <nvs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#define TAG "WifiBoard"

void WifiBoard::InitializeNetwork() {
    if (wifi_started_) {
        return;
    }

    // SoftAP 名称：Node-XXXXXX（避免同型号设备热点名冲突）
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ssid[16];
    snprintf(ssid, sizeof(ssid), "Node-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_ssid_ = ssid;

    wifi_events_ = xEventGroupCreate();
    if (wifi_events_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, this, nullptr));

    // 凭据由本类用自有 NVS 命名空间管理，WiFi 驱动不落盘
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started_ = true;

    // 时区：中国标准时间 UTC+8（SNTP 校时在拿到 IP 后启动）
    setenv("TZ", "CST-8", 1);
    tzset();

    BaseType_t ret = xTaskCreate(WifiTask, "wifi_mgr", 4096, this, 4, nullptr);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi task");
    }
}

void WifiBoard::RequestProvisioning() {
    provision_request_ = true;
}

void WifiBoard::WifiEventHandler(void* arg, esp_event_base_t base,
                                 int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiBoard*>(arg);
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(self->wifi_events_, kConnectedBit);
        xEventGroupSetBits(self->wifi_events_, kDisconnectedBit);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(self->wifi_events_, kDisconnectedBit);
        xEventGroupSetBits(self->wifi_events_, kConnectedBit);
        self->OnGotIp(static_cast<ip_event_got_ip_t*>(event_data)->ip_info.ip.addr);
    }
}

// 首次拿到 IP：启动 SNTP 校时；SSID/IP 推送到状态栏副标签
void WifiBoard::OnGotIp(uint32_t ip_netorder) {
    if (!sntp_started_) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
        sntp_started_ = true;
        ESP_LOGI(TAG, "sntp started");
    }

    char ip[16];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip_netorder);
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);

    wifi_config_t cfg = {};
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    char ssid[33] = "";
    strncpy(ssid, reinterpret_cast<const char*>(cfg.sta.ssid), sizeof(ssid) - 1);

    ESP_LOGI(TAG, "network: %s %s", ssid, ip);
    if (Display* display = GetDisplay(); display != nullptr) {
        display->UpdateNetworkInfo(ssid, ip);
    }
}

// ---------------- NVS 凭据（namespace "node"）----------------

bool WifiBoard::LoadCredentials(std::string& ssid, std::string& password) {
    nvs_handle_t handle;
    if (nvs_open("node", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t ssid_len = 0;
    size_t pass_len = 0;
    bool ok = false;
    if (nvs_get_str(handle, "wifi_ssid", nullptr, &ssid_len) == ESP_OK &&
        nvs_get_str(handle, "wifi_pass", nullptr, &pass_len) == ESP_OK &&
        ssid_len > 1 && ssid_len <= 33 && pass_len <= 65) {
        std::vector<char> s(ssid_len);
        std::vector<char> p(pass_len);
        if (nvs_get_str(handle, "wifi_ssid", s.data(), &ssid_len) == ESP_OK &&
            nvs_get_str(handle, "wifi_pass", p.data(), &pass_len) == ESP_OK) {
            ssid = s.data();
            password = p.data();
            ok = !ssid.empty();
        }
    }
    nvs_close(handle);
    return ok;
}

void WifiBoard::SaveCredentials(const std::string& ssid, const std::string& password) {
    nvs_handle_t handle;
    if (nvs_open("node", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    nvs_set_str(handle, "wifi_ssid", ssid.c_str());
    nvs_set_str(handle, "wifi_pass", password.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

// ---------------- STA 连接 ----------------

bool WifiBoard::ConnectToAp(const std::string& ssid, const std::string& password,
                            int timeout_ms) {
    wifi_config_t sta_config = {};
    strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), ssid.c_str(),
            sizeof(sta_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(sta_config.sta.password), password.c_str(),
            sizeof(sta_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    xEventGroupClearBits(wifi_events_, kConnectedBit | kDisconnectedBit);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return false;
    }

    // 预算内循环等待；每 6s 无进展则重新发起（处理事件丢失/认证超时）
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    TickType_t last_attempt = xTaskGetTickCount();
    while (xTaskGetTickCount() < deadline) {
        EventBits_t bits = xEventGroupWaitBits(wifi_events_,
                                               kConnectedBit | kDisconnectedBit,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & kConnectedBit) {
            return true;
        }
        if (xTaskGetTickCount() - last_attempt >= pdMS_TO_TICKS(6000)) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(50));
            if (esp_wifi_connect() != ESP_OK) {
                return false;
            }
            last_attempt = xTaskGetTickCount();
        }
    }
    return false;
}

// ---------------- 配网模式 ----------------

void WifiBoard::EnterProvisioning() {
    if (provisioning_.exchange(true)) {
        return;  // 已在配网模式
    }
    ShowStatus("配网模式");

    // 开放网络 SoftAP：captive portal 开箱即用
    wifi_config_t ap_config = {};
    strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid_.c_str(),
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = ap_ssid_.size();
    ap_config.ap.channel = 6;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    esp_err_t err = portal_.Start([this](const std::string& ssid, const std::string& pass) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_ssid_ = ssid;
            pending_password_ = pass;
        }
        xEventGroupSetBits(wifi_events_, kSubmitBit);
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal start failed: %s", esp_err_to_name(err));
        return;
    }

    // 主界面状态栏提示热点信息（不切屏，湿度和风扇控制保持可用）
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    char url[24];
    if (ap_netif != nullptr) {
        esp_netif_ip_info_t info = {};
        esp_netif_get_ip_info(ap_netif, &info);
        snprintf(url, sizeof(url), IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(url, sizeof(url), "192.168.4.1");
    }
    if (Display* display = GetDisplay(); display != nullptr) {
        display->UpdateNetworkInfo(ap_ssid_.c_str(), url);
    }
    ESP_LOGI(TAG, "provisioning: connect ap '%s' then open http://%s",
             ap_ssid_.c_str(), url);
}

void WifiBoard::ExitProvisioning() {
    if (!provisioning_.exchange(false)) {
        return;
    }
    portal_.Stop();
    // APSTA -> STA：先断开 STA 再切模式，随后由状态机重新连接
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    vTaskDelay(pdMS_TO_TICKS(100));
}

void WifiBoard::ShowStatus(const char* status) {
    if (Display* display = GetDisplay(); display != nullptr) {
        display->SetStatus(status);
    }
}

// ---------------- 状态机任务 ----------------

void WifiBoard::WifiTask(void* arg) {
    auto* self = static_cast<WifiBoard*>(arg);
    std::string ssid;
    std::string password;
    bool provisioning = false;

    if (self->LoadCredentials(ssid, password)) {
        self->ShowStatus("连接中...");
        if (self->ConnectToAp(ssid, password, kConnectTimeoutMs)) {
            ESP_LOGI(TAG, "connected to %s", ssid.c_str());
            self->ShowStatus("已连接");
        } else {
            ESP_LOGW(TAG, "connect timeout, entering provisioning mode");
            self->EnterProvisioning();
            provisioning = true;
        }
    } else {
        ESP_LOGW(TAG, "no saved credentials, entering provisioning mode");
        self->EnterProvisioning();
        provisioning = true;
    }

    TickType_t next_retry = 0;
    while (true) {
        if (provisioning) {
            // ---- 配网模式：网页提交凭据 -> 验证 -> 成功退出 ----
            EventBits_t bits = xEventGroupWaitBits(self->wifi_events_, kSubmitBit,
                                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            if (bits & kSubmitBit) {
                std::string submit_ssid;
                std::string submit_pass;
                {
                    std::lock_guard<std::mutex> lock(self->pending_mutex_);
                    submit_ssid = self->pending_ssid_;
                    submit_pass = self->pending_password_;
                }
                ESP_LOGI(TAG, "credentials submitted for %s", submit_ssid.c_str());
                self->portal_.SetStatus(PortalStatus::kConnecting);
                if (self->ConnectToAp(submit_ssid, submit_pass, kVerifyTimeoutMs)) {
                    self->SaveCredentials(submit_ssid, submit_pass);
                    ssid = submit_ssid;
                    password = submit_pass;
                    self->portal_.SetStatus(PortalStatus::kSuccess);
                    vTaskDelay(pdMS_TO_TICKS(5000));  // 留时间给网页展示成功状态
                    self->ExitProvisioning();
                    // 切回纯 STA 模式后重连（刚验证过，通常秒连）
                    self->ShowStatus("连接中...");
                    if (self->ConnectToAp(ssid, password, kConnectTimeoutMs)) {
                        self->ShowStatus("已连接");
                        provisioning = false;
                        ESP_LOGI(TAG, "provisioning done, connected to %s", ssid.c_str());
                    } else {
                        self->EnterProvisioning();  // 极少见：退回配网
                    }
                    continue;
                }
                self->portal_.SetStatus(PortalStatus::kFailed);
            }

            // ---- 后台静默重试已保存凭据（路由器重启自愈）----
            TickType_t now = xTaskGetTickCount();
            if (now >= next_retry) {
                next_retry = now + pdMS_TO_TICKS(kRetryIntervalMs);
                std::string saved_ssid;
                std::string saved_pass;
                if (self->LoadCredentials(saved_ssid, saved_pass) &&
                    self->ConnectToAp(saved_ssid, saved_pass, kRetryTimeoutMs)) {
                    ssid = saved_ssid;
                    password = saved_pass;
                    self->ExitProvisioning();
                    self->ShowStatus("连接中...");
                    if (self->ConnectToAp(ssid, password, kConnectTimeoutMs)) {
                        self->ShowStatus("已连接");
                        provisioning = false;
                        ESP_LOGI(TAG, "auto recovered to %s", ssid.c_str());
                    } else {
                        self->EnterProvisioning();
                    }
                    continue;
                }
            }

            if (self->provision_request_.exchange(false)) {
                // 已在配网模式，忽略重复请求
            }
        } else {
            // ---- 已联网：处理手动配网请求与掉线 ----
            if (self->provision_request_.exchange(false)) {
                ESP_LOGI(TAG, "manual provisioning requested");
                self->ShowStatus("配网模式");
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(200));
                self->EnterProvisioning();
                provisioning = true;
                continue;
            }

            EventBits_t bits = xEventGroupWaitBits(self->wifi_events_, kDisconnectedBit,
                                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
            if (bits & kDisconnectedBit) {
                self->ShowStatus("重连中...");
                if (self->ConnectToAp(ssid, password, kConnectTimeoutMs)) {
                    ESP_LOGI(TAG, "reconnected to %s", ssid.c_str());
                    self->ShowStatus("已连接");
                } else {
                    ESP_LOGW(TAG, "reconnect timeout, entering provisioning mode");
                    self->EnterProvisioning();
                    provisioning = true;
                }
            }
        }
    }
}
