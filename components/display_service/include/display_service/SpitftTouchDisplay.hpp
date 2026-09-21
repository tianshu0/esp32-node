// SpitftTouchDisplay：SPI TFT + 电阻式触摸的彩色显示驱动
//
// 与现有 Ssd1315Display（I2C OLED 单色单屏）不同：
//   - SPI 接口驱动 ILI9341，分辨率 240x320（竖屏）或 320x240（横屏）
//   - XPT2046 SPI 电阻式触摸（独立 SPI3_HOST，4 线不与 LCD 共享）
//   - PSRAM 可存整屏双缓冲，刷新流畅
//   - 触摸输入通过 lv_indev 注册到 LVGL（LVGL 9 新 API）
//   - 内置多页面 UI：Dashboard（仪表板）/ Automation（规则）/ Settings（设置）
//   - 风扇状态实时显示
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

    // LVGL 9 输入设备回调签名：(lv_indev_t*, lv_indev_data_t*)
    static void IndevRead(lv_indev_t* indev, lv_indev_data_t* data);

    // ==================== 成员 ====================
    DisplayContext ctx_{};
    FanControl* fan_ = nullptr;
    SpitftPins pins_{};
    SpitftConfig cfg_{};

    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    lv_disp_t* lv_disp_ = nullptr;

    // LVGL 屏幕对象（多页）
    lv_obj_t* scr_dashboard_  = nullptr;
    lv_obj_t* scr_automation_ = nullptr;
    lv_obj_t* scr_settings_   = nullptr;

    // Dashboard 实时数据标签
    lv_obj_t* lbl_temp_value_ = nullptr;
    lv_obj_t* lbl_humi_value_ = nullptr;

    // 触摸最后有效点：抬起时必须继续上报，不能给 (-1,-1)，
    // 否则 LVGL 把点击判为超距滑动而取消 CLICK
    int16_t last_touch_x_ = 0;
    int16_t last_touch_y_ = 0;
    bool    touch_has_point_ = false;

    // 周期刷新任务（每秒更新 Dashboard 数据）
    TaskHandle_t task_ = nullptr;
    static void RefreshTask(void* arg);
};

} // namespace esp32node
