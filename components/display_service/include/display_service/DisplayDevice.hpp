// 显示驱动基类：只管「什么屏」——初始化面板、启动 LVGL、周期把数据交给 Screen
//
// 子类负责具体面板（SSD1315 / 将来的 ST7789 等）；显示内容（布局/字段）由 Screen
// 决定，驱动不认识任何具体传感器字段。屏未接时 Start() 返回错误，装配层告警跳过。
#pragma once

#include "esp_err.h"
#include "display_service/DisplayContext.hpp"

namespace esp32node {

class DisplayDevice {
public:
    virtual ~DisplayDevice() = default;

    // 初始化面板 + LVGL + 内容模板 + 启动周期刷新任务（任务在组件内部创建）
    virtual esp_err_t Start(const DisplayContext& ctx) = 0;
};

} // namespace esp32node
