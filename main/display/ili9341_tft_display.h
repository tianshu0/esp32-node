#pragma once

#include "display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <cstdint>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_NODE_TOUCH_XPT2046
#include <esp_lcd_touch.h>
#endif

// ILI9341 SPI TFT 显示类（240x320 RGB565）
// 板级负责：SPI 总线 / panel_io / panel / 触摸句柄的创建与初始化；
// 本类只负责 LVGL 端口注册、UI 布局与数据更新（对标 Ssd1315OledDisplay）。
class Ili9341TftDisplay : public Display {
public:
    Ili9341TftDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, bool mirror_x, bool mirror_y
#if CONFIG_NODE_TOUCH_XPT2046
                      ,
                      esp_lcd_touch_handle_t touch_handle = nullptr
#endif
    );
    ~Ili9341TftDisplay();

    virtual void SetupUI() override;
    virtual void SetStatus(const char* status) override;

#if CONFIG_BOARD_ESP32S3_TFT_FAN
    // 注入风扇开关动作（LEDC 由板级控制，显示类不认识风扇硬件）
    void SetFanToggleCallback(std::function<void(bool)> callback);

    // 推送温湿度数据；传感器缺失时值传 NAN，界面显示 "--"
    // （V2 布局仅展示湿度与风扇状态，temperature_c 预留）
    void UpdateEnv(float temperature_c, float humidity_pct);

    // 同步风扇运行状态（板级也可在外部状态变化时主动刷新按钮）
    void SetFanRunning(bool running);
#endif

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_240x320();

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* lv_display_ = nullptr;
#if CONFIG_NODE_TOUCH_XPT2046
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
#endif

#if CONFIG_BOARD_ESP32S3_TFT_FAN
    // ---- 顶部状态栏 ----
    lv_obj_t* conn_dot_ = nullptr;
    lv_obj_t* conn_label_ = nullptr;
    lv_obj_t* wifi_st_label_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;

    // ---- 信息卡片：湿度 / 风扇状态 ----
    lv_obj_t* humi_value_label_ = nullptr;
    lv_obj_t* humi_pct_label_ = nullptr;
    lv_obj_t* humi_status_label_ = nullptr;
    lv_obj_t* fan_state_label_ = nullptr;

    // ---- 底部大按钮 ----
    lv_obj_t* btn_fan_ = nullptr;

    bool fan_running_ = false;
    std::function<void(bool)> fan_callback_;

    // 文本去重缓存：湿度不变时跳过 set_text，避免无谓重绘
    char humi_cache_[16] = {};

    static void FanButtonEventHandler(lv_event_t* event);
    void ToggleFan();
    void RefreshFanButton();

    // 内部 1s 任务：只更新时间/日期（与传感器无关）
    static void StatusTask(void* arg);
    TaskHandle_t status_task_ = nullptr;
#endif
};
