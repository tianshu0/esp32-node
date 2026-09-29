// VocDisplay：c3_oled_voc 板载显示（SSD1315 128x64 OLED + 21VOC 五合一布局）
//
// 与 ThpDisplay 同面板同布局，差异仅在字段映射（TVOC/CH2O/eCO2/温度/湿度）
#pragma once

#include "display/display.hpp"
#include "i2c_bus/i2c_bus.hpp"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include <cstdint>

namespace esp32node {

class VocDisplay : public Display {
public:
    VocDisplay(I2cBus* i2c, uint8_t addr, int w, int h, bool mirror_x, bool mirror_y);
    ~VocDisplay() override = default;

    void SetStatus(const char* status) override;
    void UpdateSamples(const SensorReading* samples, int count) override;

    // Board 装配层调用（构造后初始化 UI）
    void BuildUi(const char* node_id);

    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    void UpdateRows(const SensorReading* samples, int count);

    struct Row {
        lv_obj_t* value = nullptr;
        char cache[20] = {};
    };

    I2cBus* i2c_ = nullptr;
    uint8_t addr_ = 0;
    bool mirror_x_ = false;
    bool mirror_y_ = false;

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* disp_ = nullptr;

    lv_obj_t* status_label_ = nullptr;
    char status_cache_[12] = {};
    Row rows_[5] = {};  // TVOC/CH2O/eCO2/温度/湿度
};

} // namespace esp32node
