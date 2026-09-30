// ThpDisplay：c3_oled_thp 板载显示（SSD1315 128x64 OLED + SHT3X/BMP180 布局）
//
// 合并原 Ssd1315Display + DashboardScreen 职责：
//   - 初始化 SSD1315 面板（I2C）+ LVGL 单色 I1 端口
//   - 构建温湿度气压三行布局（传感器 type 写死为 "sht3x"/"bmp180"）
//   - UpdateSamples 由 Application 1s 任务推数据（原 Run 任务迁移）
//   - SetStatus 显示 BLE 连接状态（ADV/CONN/PAIRED）
//
// 构造注入：I2C 总线句柄、面板地址、宽高、镜像配置由 Board 装配层传入。
#pragma once

#include "display/display.hpp"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include <cstdint>

namespace esp32node {

class ThpDisplay : public Display {
public:
    // 构造即创建面板与 LVGL，失败返回 ESP_ERR_NOT_FOUND
    // 生命周期：与 Board 相同（常驻到重启）
    ThpDisplay(i2c_master_bus_handle_t bus, uint8_t addr, int w, int h, bool mirror_x, bool mirror_y);
    ~ThpDisplay() override = default;

    // Display 被动接口
    void SetStatus(const char* status) override;
    void UpdateSamples(const SensorReading* samples, int count) override;

    // Board 装配层调用（构造后初始化 UI）
    void BuildUi(const char* node_id);

    // 分辨率（子类填充）
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    void UpdateRows(const SensorReading* samples, int count);  // 更新数值行

    // 行结构：温/湿/气压三行
    struct Row {
        lv_obj_t* value = nullptr;
        char cache[20] = {};  // 文本去重，避免整屏 I2C 重绘
    };

    i2c_master_bus_handle_t bus_ = nullptr;
    uint8_t addr_ = 0;
    bool mirror_x_ = false;
    bool mirror_y_ = false;

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* disp_ = nullptr;

    // UI 对象
    lv_obj_t* status_label_ = nullptr;
    char status_cache_[12] = {};
    Row rows_[3] = {};  // 温/湿/气压
};

} // namespace esp32node
