// Ssd1306 0.91" 128x32 单色 OLED 显示驱动（DisplayDevice 实现）
//
// 与 Ssd1315Display（128x64）同属 SSD1306/SSD1315 控制器家族，仅分辨率不同。
// esp_lcd 的 ssd1306 面板驱动通过 height 参数支持 32 行；初始化序列与 64 行版一致。
// 显示内容由 DisplayContext.screen 指向的 Screen 模板决定（128x32 配 CompactDashboardScreen）。
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

class Ssd1306Display : public DisplayDevice {
public:
    static constexpr uint8_t kAddrPrimary = 0x3C;
    static constexpr uint8_t kAddrAlt = 0x3D;
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 32;
    static constexpr uint32_t kRefreshMs = 1000;
    static constexpr uint32_t kTaskStack = 4096;
    static constexpr UBaseType_t kTaskPriority = 4;

    ~Ssd1306Display() override;

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
