#include "Application.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"

#if CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#endif

namespace esp32node {

static const char* TAG = "esp32-node";

esp_err_t Application::Init()
{
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    // NVS 必须先于 Wi-Fi 初始化：phy 校准数据的读写要走 NVS，否则会报
    // "esp_phy_load_cal_data_from_nvs: NVS has not been initialized" 并退回全量校准。
    ESP_RETURN_ON_ERROR(config_.Init(), TAG, "app config init failed");

#if CONFIG_BT_ENABLED
    // NimBLE 协议栈初始化（host 必须先于 ble_peripheral 启动）
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");
#endif

    ESP_RETURN_ON_ERROR(registry_.Init(), TAG, "sensor registry init failed");

    // data_pipeline 持有事件基，display/automation/ble 订阅该事件基投递的采集数据
    ESP_RETURN_ON_ERROR(pipeline_.Init(&config_, &registry_), TAG, "pipeline init failed");

#if CONFIG_BT_ENABLED
    ESP_RETURN_ON_ERROR(ble_.Init(&config_, &registry_), TAG, "ble init failed");
#endif

    // 板级装配：创建总线、勾选的传感器、选中的显示屏（屏为可选项，失败只告警）
    board_ctx_.config = &config_;
    board_ctx_.registry = &registry_;
#if CONFIG_BT_ENABLED
    board_ctx_.ble = &ble_;
#endif
    board_ = &GetBoard(board_ctx_);
    board_->Assemble();

    power_.Init(&config_);

    // portal 依赖装配输出（fan 指针），必须在 Assemble 之后
    ESP_RETURN_ON_ERROR(portal_.Init(&config_, board_->GetFan(), &registry_),
                        TAG, "wifi portal init failed");

    return ESP_OK;
}

void Application::Run()
{
    ESP_LOGI(TAG, "esp32-node started: board=%s id=%s i2c(sda=%d scl=%d) sensors=%d display=%s",
             board_->Name(),
             config_.NodeId().c_str(), config_.I2cSda(), config_.I2cScl(),
             registry_.Count(), board_->DisplayPresent() ? "on" : "off");
}

} // namespace esp32node
