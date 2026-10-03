#include "ssd1306_oled_display.h"

#include <esp_lvgl_port.h>

#define TAG "Ssd1306OledDisplay"

Ssd1306OledDisplay::Ssd1306OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {

    width_ = width;
    height_ = height;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        // C++ 指定初始化器必须按结构体声明顺序：rotation 在 color_format 之前
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_I1,
#endif
        .flags =
            {
                .buff_dma = 0,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

}

Ssd1306OledDisplay::~Ssd1306OledDisplay() {
}

bool Ssd1306OledDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void Ssd1306OledDisplay::Unlock() {
    lvgl_port_unlock();
}

void Ssd1306OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t* label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_label_set_text(label, "esp32-node");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 1);
}

void Ssd1306OledDisplay::SetupUI() {
    Display::SetupUI();
    SetupUI_128x64();
}
