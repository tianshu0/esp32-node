// Ssd1315 0.96" 128x64 单色 OLED 显示驱动（DisplayDevice 实现）
//
// 仅负责：I2C 面板（esp_lcd，兼容 SSD1306）、LVGL 9 + esp_lvgl_port 单色 I1、
// 周期刷新任务。显示内容由 DisplayContext.screen 指向的 Screen 模板决定。
//
// 数据来源：每 1s 通过 sensor_registry 主动采集一次（未配对时也有实时值，
// 因此不订阅 data_pipeline 只在配对后采样的事件）。
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "display_service/DisplayDevice.hpp"
#include "display_service/DisplayContext.hpp"

namespace esp32node {

class Ssd1315Display : public DisplayDevice {
public:
    static constexpr uint8_t kAddrPrimary = 0x3C;
    static constexpr uint8_t kAddrAlt = 0x3D;
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;
    static constexpr uint32_t kRefreshMs = 1000;
    static constexpr uint32_t kTaskStack = 4096;
    static constexpr UBaseType_t kTaskPriority = 4;

    ~Ssd1315Display() override;

    // 建面板 + 挂 LVGL + 构建 Screen + 启动刷新任务。
    // 屏未接（0x3C/0x3D 均无应答）返回 ESP_ERR_NOT_FOUND，装配方可忽略。
    esp_err_t Start(const DisplayContext& ctx) override;

private:
    esp_err_t SetupPanel();
    const char* StatusText() const;

    static void TaskThunk(void* ctx);
    void Run();

    DisplayContext ctx_ = {};

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* disp_ = nullptr;
    uint8_t addr_ = kAddrPrimary;

    TaskHandle_t task_ = nullptr;
};

} // namespace esp32node
