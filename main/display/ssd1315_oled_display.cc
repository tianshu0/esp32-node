#include "ssd1315_oled_display.h"

#include <esp_lvgl_port.h>

#define TAG "Ssd1315OledDisplay"

Ssd1315OledDisplay::Ssd1315OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
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
        // （官方 i2c_oled 示例是 .c，C 允许乱序，不能照抄其顺序）。
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
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

Ssd1315OledDisplay::~Ssd1315OledDisplay() {
}

bool Ssd1315OledDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void Ssd1315OledDisplay::Unlock() {
    lvgl_port_unlock();
}


void Ssd1315OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // ---- 页眉：node-id（左）+ 连接状态（右）----
    lv_obj_t* node_label = lv_label_create(screen);
    lv_obj_set_style_text_font(node_label, &lv_font_montserrat_10, 0);
    lv_label_set_text(node_label, "esp32-node");
    lv_obj_set_pos(node_label, 2, 1);

    status_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_10, 0);
    lv_label_set_text(status_label_, "ready");
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, -2, 1);

    lv_obj_t* divider = lv_obj_create(screen);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, width_, 1);
    lv_obj_set_pos(divider, 0, 13);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
}

void Ssd1315OledDisplay::SetupUI() {
    Display::SetupUI();
    SetupUI_128x64();
}
