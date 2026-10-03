// FanDisplay：s3_tft_fan 板载显示（ILI9341 240x320 SPI TFT + XPT2046 触摸）
//
// 合并原 SpitftTouchDisplay 职责，并做依赖反转：
//   - 不再认识 FanControl，按钮点击时调 std::function<void(bool)> 回调
//   - UpdateSamples 由 Application 1s 任务推数据（提取 humidity 字段）
//   - SetStatus 显示 WiFi 连接状态（"已连接"/"未连接"）
//   - 状态栏时间/日期仍由内部 1s 任务更新（与传感器无关）
#pragma once

#include "display/Display.hpp"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include <functional>
#include <cstdint>

#if CONFIG_NODE_TOUCH_XPT2046
#if __has_include("esp_lcd_touch.h")
#include "esp_lcd_touch.h"
#else
#include "esp_lcd_touch/esp_lcd_touch.h"
#endif
#endif

#include <lvgl.h>

namespace esp32node {

struct FanDisplayPins {
    spi_host_device_t spi_host = SPI2_HOST;
    int lcd_cs   = -1;
    int lcd_dc   = -1;
    int lcd_rst  = -1;
    int lcd_bl   = -1;
    int touch_cs = -1;
    spi_host_device_t touch_spi_host = SPI3_HOST;
};

enum class ScreenRotation : uint8_t {
    Portrait    = 0,
    Landscape   = 1,
};

struct FanDisplayConfig {
    int lcd_pclk_mhz   = 20;
    ScreenRotation rotation = ScreenRotation::Portrait;
    int screen_w = 240;
    int screen_h = 320;
    bool double_buffer = true;
    int buffer_rows = 20;
};

class FanDisplay : public Display {
public:
    // 构造即初始化面板/LVGL/触摸，fan_cb 为按钮回调（true=开 false=关）
    FanDisplay(const FanDisplayPins& pins, const FanDisplayConfig& cfg,
               std::function<void(bool)> fan_cb);
    ~FanDisplay() override = default;

    void SetStatus(const char* status) override;  // "已连接"/"未连接"
    void UpdateSamples(const SensorReading* samples, int count) override;  // 提取 humidity

    lv_disp_t* LvglDisplay() const { return lv_disp_; }

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    esp_err_t InitLcd();
    esp_err_t InitTouch();
    esp_err_t InitLvgl();
    void BuildAllPages();

    static void FanBtnHandler(lv_event_t* e);
    void UpdateFanButton(bool running);

    // 内部 1s 任务：只更新时间/日期（与传感器无关）
    static void StatusTask(void* arg);

    FanDisplayPins pins_{};
    FanDisplayConfig cfg_{};
    std::function<void(bool)> fan_cb_;

    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    lv_disp_t* lv_disp_ = nullptr;

#if CONFIG_NODE_TOUCH_XPT2046
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
#endif

    lv_obj_t* scr_main_ = nullptr;

    // 状态栏
    lv_obj_t* lbl_conn_      = nullptr;
    lv_obj_t* dot_conn_      = nullptr;
    lv_obj_t* lbl_wifi_st_  = nullptr;
    lv_obj_t* lbl_time_     = nullptr;
    lv_obj_t* lbl_date_     = nullptr;

    // 湿度/风扇状态
    lv_obj_t* lbl_humi_value_  = nullptr;
    lv_obj_t* lbl_humi_pct_    = nullptr;
    lv_obj_t* arc_humi_        = nullptr;
    lv_obj_t* lbl_arc_pct_     = nullptr;
    lv_obj_t* lbl_humi_status_ = nullptr;

    // 底部按钮/状态
    lv_obj_t* btn_fan_           = nullptr;
    lv_obj_t* lbl_fan_switch_    = nullptr;
    lv_obj_t* lbl_fan_switch_st_ = nullptr;
    lv_obj_t* fan_state_card_    = nullptr;
    lv_obj_t* lbl_fan_state_st_  = nullptr;

    bool fan_running_ = false;  // 按钮回调后更新，UpdateFanButton 使用
    TaskHandle_t status_task_ = nullptr;
};

} // namespace esp32node
