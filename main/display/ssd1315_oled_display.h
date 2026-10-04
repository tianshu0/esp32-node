#pragma once

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <cstdint>

class Ssd1315OledDisplay : public Display {
private:
    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;

    lv_obj_t* status_label_ = nullptr;

#if CONFIG_BOARD_ESP32C3_SHT3X
    lv_obj_t* temp_value_label_ = nullptr;
    lv_obj_t* humi_value_label_ = nullptr;
    // 文本去重缓存：数值不变时跳过 lv_label_set_text，避免整屏 I2C 重绘
    char temp_cache_[16] = {};
    char humi_cache_[16] = {};
#endif

#if CONFIG_BOARD_ESP32C3_VOC21
    lv_obj_t* tvoc_value_label_ = nullptr;
    lv_obj_t* ch2o_value_label_ = nullptr;
    // 文本去重缓存：数值不变时跳过 lv_label_set_text，避免整屏 I2C 重绘
    char tvoc_cache_[16] = {};
    char ch2o_cache_[16] = {};
#endif

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();

protected:
    lv_disp_t* display_ = nullptr;

public:
    Ssd1315OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~Ssd1315OledDisplay();

    virtual void SetupUI() override;

#if CONFIG_BOARD_ESP32C3_SHT3X
    // 更新 SHT3X 温湿度数值（需在 SetupUI 之后调用；内部自持有 LVGL 锁）
    void UpdateSht3x(float temperature_c, float humidity_pct);
#endif

#if CONFIG_BOARD_ESP32C3_VOC21
    // 更新 21VOC 的 TVOC/甲醛数值（ug/m3，整数；内部自持有 LVGL 锁）
    void UpdateVoc21(uint16_t tvoc_ug_m3, uint16_t ch2o_ug_m3);
#endif
};
