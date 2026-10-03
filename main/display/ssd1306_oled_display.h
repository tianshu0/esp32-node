#pragma once

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

class Ssd1306OledDisplay : public Display {
private:
    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;

    lv_obj_t* status_label_ = nullptr;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();

protected:
    lv_display_t* display_ = nullptr;

public:
    Ssd1306OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~Ssd1306OledDisplay();

    virtual void SetupUI() override;
};