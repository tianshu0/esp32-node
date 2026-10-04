#include "ssd1315_oled_display.h"

#include <esp_lvgl_port.h>
#include "fonts.h"
#include "icons.h"

#include <cstdio>
#include <cstring>

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

    // ---- 页眉：node-id（左）+ 状态徽章（右，圆角线框）----
    lv_obj_t* node_label = lv_label_create(screen);
    lv_obj_set_style_text_font(node_label, &lv_font_montserrat_10, 0);
    lv_label_set_text(node_label, "esp32-node");
    lv_obj_set_pos(node_label, 2, 1);

    lv_obj_t* badge = lv_obj_create(screen);
    lv_obj_remove_style_all(badge);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(badge, 46, 13);
    lv_obj_set_pos(badge, 80, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_black(), 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 3, 0);

    status_label_ = lv_label_create(badge);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_10, 0);
    lv_label_set_text(status_label_, "Ready");
    lv_obj_center(status_label_);

    // 页眉分隔线
    lv_obj_t* divider = lv_obj_create(screen);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, width_, 1);
    lv_obj_set_pos(divider, 0, 13);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

#if CONFIG_BOARD_ESP32C3_SHT3X
    // ---- 双卡片：温度（左）/ 湿度（右），61x44 圆角线框 ----
    // 卡片内：上行 图标 + 中文标签，下行 大数字（18px 纯数字字体）+ 小单位
    const struct {
        const lv_image_dsc_t* icon;
        const char* tag;
        const char* unit;
        lv_obj_t** value;
    } kCards[] = {
        { &icon_thermo,  "温度", "°C", &temp_value_label_ },
        { &icon_droplet, "湿度", "%",  &humi_value_label_ },
    };

    for (int i = 0; i < 2; ++i) {
        const int cx = 1 + i * 65;  // 卡片左缘：1 / 66

        lv_obj_t* card = lv_obj_create(screen);
        lv_obj_remove_style_all(card);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(card, 61, 44);
        lv_obj_set_pos(card, cx, 15);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 3, 0);

        lv_obj_t* icon = lv_image_create(card);
        lv_image_set_src(icon, kCards[i].icon);
        lv_obj_set_pos(icon, 4, 4);

        lv_obj_t* tag = lv_label_create(card);
        lv_obj_set_style_text_font(tag, &lv_font_zh12, 0);
        lv_label_set_text(tag, kCards[i].tag);
        lv_obj_set_pos(tag, 23, 6);

        lv_obj_t* unit = lv_label_create(card);
        lv_obj_set_style_text_font(unit, &lv_font_zh14, 0);
        lv_label_set_text(unit, kCards[i].unit);
        lv_obj_align(unit, LV_ALIGN_BOTTOM_RIGHT, -2, -4);

        lv_obj_t* value = lv_label_create(card);
        lv_obj_set_style_text_font(value, &lv_font_num18, 0);
        lv_label_set_text(value, "--");
        // 数字右对齐到卡片内，右侧留出 14px 给单位（避免过宽时盖住 °C / %）
        lv_obj_align(value, LV_ALIGN_BOTTOM_RIGHT, -14, -3);
        *kCards[i].value = value;
    }
#endif

    // ---- 底部装饰：线 + /// + 线 ----
    lv_obj_t* foot_l = lv_obj_create(screen);
    lv_obj_remove_style_all(foot_l);
    lv_obj_set_size(foot_l, 55, 1);
    lv_obj_set_pos(foot_l, 1, 61);
    lv_obj_set_style_bg_color(foot_l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(foot_l, LV_OPA_COVER, 0);

    lv_obj_t* foot_r = lv_obj_create(screen);
    lv_obj_remove_style_all(foot_r);
    lv_obj_set_size(foot_r, 49, 1);
    lv_obj_set_pos(foot_r, 79, 61);
    lv_obj_set_style_bg_color(foot_r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(foot_r, LV_OPA_COVER, 0);

    for (int i = 0; i < 3; ++i) {
        static const lv_point_precise_t slash_pts[] = { {0, 5}, {3, 0} };
        lv_obj_t* slash = lv_line_create(screen);
        lv_line_set_points(slash, slash_pts, 2);
        lv_obj_set_pos(slash, 62 + i * 5, 58);
        lv_obj_set_style_line_width(slash, 1, 0);
        lv_obj_set_style_line_color(slash, lv_color_black(), 0);
    }
}

#if CONFIG_BOARD_ESP32C3_SHT3X
void Ssd1315OledDisplay::UpdateSht3x(float temperature_c, float humidity_pct) {
    if (temp_value_label_ == nullptr || humi_value_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);

    // 数值标签只放数字，单位是卡片上的静态小字
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(temperature_c));
    if (std::strcmp(temp_cache_, text) != 0) {
        std::strncpy(temp_cache_, text, sizeof(temp_cache_) - 1);
        lv_label_set_text(temp_value_label_, temp_cache_);
    }

    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(humidity_pct));
    if (std::strcmp(humi_cache_, text) != 0) {
        std::strncpy(humi_cache_, text, sizeof(humi_cache_) - 1);
        lv_label_set_text(humi_value_label_, humi_cache_);
    }
}
#endif

void Ssd1315OledDisplay::SetupUI() {
    Display::SetupUI();
    SetupUI_128x64();
}
