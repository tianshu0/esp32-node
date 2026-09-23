// SpitftTouchDisplay：SPI TFT + 电阻式触摸的彩色显示驱动
//
// 与现有 Ssd1315Display（I2C OLED 单色单屏）不同：
//   - SPI 接口驱动 ILI9341，分辨率 240x320（竖屏）或 320x240（横屏）
//   - XPT2046 SPI 电阻式触摸（独立 SPI3_HOST，4 线不与 LCD 共享）
//   - PSRAM 可存整屏双缓冲，刷新流畅
//   - 触摸输入通过 lv_indev 注册到 LVGL（LVGL 9 新 API）
//   - 单屏极简 UI：湿度显示 + 一键启停风扇大按钮
//     （自动化规则由 App 经 MQTT 下发，屏幕上不做规则/设置入口）
//   - 按钮外观每秒与 FanControl 实际状态同步，规则/MQTT 改动能自动反映
//
// 用法（板型装配层）：
//   display.Configure(tft_pins, &fan);  // 先放硬件参数
//   display.Start(dctx);                 // 再走 DisplayDevice 标准入口
#pragma once

#include "display_service/DisplayDevice.hpp"
#include "display_service/DisplayContext.hpp"
#include "fan_control/FanControl.hpp"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_NODE_TOUCH_XPT2046
// esp_lcd_touch 1.x 头在 esp_lcd_touch/ 子目录，2.x 移到了 include 根
#if __has_include("esp_lcd_touch.h")
#include "esp_lcd_touch.h"
#else
#include "esp_lcd_touch/esp_lcd_touch.h"
#endif
#endif
#include <lvgl.h>

namespace esp32node {

struct SpitftPins {
    spi_host_device_t spi_host = SPI2_HOST;
    int lcd_cs   = -1;
    int lcd_dc   = -1;
    int lcd_rst  = -1;
    int lcd_bl   = -1;    // 背光脚（-1 表示固定接 3V3）
    int touch_cs = -1;    // XPT2046 片选
    // XPT2046 所在 SPI 主机：可与 LCD 共享 SPI2，也可独立用 SPI3
    spi_host_device_t touch_spi_host = SPI3_HOST;
};

// 屏幕方向
enum class ScreenRotation : uint8_t {
    Portrait    = 0,  // 240x320（排针朝下安装，MADCTL=BGR|MX=0x48）
    Landscape   = 1,  // 320x240（MV=1）
};

struct SpitftConfig {
    // SPI 像素时钟：杜邦飞线 + ILI9341 模组建议 20MHz；40MHz 仅在短 PCB 走线
    // （如 hub 板载连接）下稳定，飞线 40MHz 会因信号反射导致像素流失位、
    // 画面出现周期性水平条纹/错位。显示稳定后可尝试 26/40MHz 逐步找上限。
    int lcd_pclk_mhz   = 20;
    ScreenRotation rotation = ScreenRotation::Portrait; // 竖屏：排针朝下安装
    int screen_w = 240;
    int screen_h = 320;
    bool double_buffer = true;
    int buffer_rows = 20;  // 分区双缓冲：240*20*2 = 9600B/缓冲
};

class SpitftTouchDisplay : public DisplayDevice {
public:
    SpitftTouchDisplay() = default;
    ~SpitftTouchDisplay() = default;

    // 装配层在 Start() 前调用：传入引脚映射和风扇指针
    void Configure(const SpitftPins& pins, FanControl* fan_ctrl = nullptr) {
        pins_ = pins;
        fan_ = fan_ctrl;
    }

    // DisplayDevice 标准入口：初始化 SPI TFT + 触摸 + LVGL + UI
    esp_err_t Start(const DisplayContext& ctx) override;

    // 暴露 LVGL 显示对象给 Automation 等组件
    lv_disp_t* LvglDisplay() const { return lv_disp_; }

private:
    esp_err_t InitLcd();
    esp_err_t InitTouch();
    esp_err_t InitLvgl();
    void BuildAllPages();

    // 风扇按钮：点击回调 + 按 FanControl 实际状态刷新外观（调用时须持 LVGL 锁）
    static void FanBtnHandler(lv_event_t* e);
    void UpdateFanButton();

    // ==================== 成员 ====================
    DisplayContext ctx_{};
    FanControl* fan_ = nullptr;
    SpitftPins pins_{};
    SpitftConfig cfg_{};

    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    lv_disp_t* lv_disp_ = nullptr;

#if CONFIG_NODE_TOUCH_XPT2046
    // XPT2046：标准 esp_lcd_touch 句柄（atanisoft 驱动）+ 其 SPI IO
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
#endif

    // LVGL 屏幕对象（单屏，参考图风格）
    lv_obj_t* scr_main_ = nullptr;

    // ===== WiFi/时间状态栏 =====
    lv_obj_t* lbl_conn_      = nullptr;  // "已连接"/"未连接"
    lv_obj_t* dot_conn_      = nullptr;  // 状态指示圆点（V2）
    lv_obj_t* lbl_wifi_st_  = nullptr;  // STA IP
    lv_obj_t* lbl_time_     = nullptr;  // "--:--"
    lv_obj_t* lbl_date_     = nullptr;  // 日期（V1 用，V2 不用）

    // ===== 湿度/风扇状态 =====
    lv_obj_t* lbl_humi_value_  = nullptr;
    lv_obj_t* arc_humi_        = nullptr;
    lv_obj_t* lbl_arc_pct_     = nullptr;
    lv_obj_t* lbl_humi_status_ = nullptr;

    // ===== 底部按钮/状态 =====
    lv_obj_t* btn_fan_           = nullptr;
    lv_obj_t* lbl_fan_switch_    = nullptr;
    lv_obj_t* lbl_fan_switch_st_ = nullptr;
    lv_obj_t* fan_state_card_    = nullptr;
    lv_obj_t* lbl_fan_state_st_  = nullptr;

    // 周期刷新任务（每秒更新 Dashboard 数据）
    TaskHandle_t task_ = nullptr;
    static void RefreshTask(void* arg);
};

} // namespace esp32node
