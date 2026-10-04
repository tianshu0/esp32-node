#include "ili9341_tft_display.h"

#include <esp_lvgl_port.h>
#include "fonts.h"

#include <esp_log.h>
#include <esp_system.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Ili9341TftDisplay"

namespace {

// ---- 面板配色（深色青蓝仪表盘）----
constexpr uint32_t kColorBg     = 0x0F172A;
constexpr uint32_t kColorBtnOff = 0x374151;
constexpr uint32_t kColorBtnOn  = 0x10B981;

// 带状态圆点的小标签；*lbl_out 输出内部 label，外部对其父容器做 align
void MakeStatusTag(lv_obj_t* parent, const char* text, uint32_t dot_color,
                   lv_obj_t** lbl_out) {
    lv_obj_t* tag = lv_obj_create(parent);
    lv_obj_set_style_radius(tag, 12, 0);
    lv_obj_set_style_bg_color(tag, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tag, 4, 0);
    lv_obj_set_style_pad_column(tag, 5, 0);
    lv_obj_set_style_border_width(tag, 0, 0);
    lv_obj_set_size(tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tag, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tag, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tag, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* dot = lv_obj_create(tag);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(tag);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_zh14, 0);
    *lbl_out = lbl;
}

// 风扇画布（软件绘制外圆环 + 3 叶片 + 轴芯）
lv_obj_t* MakeFanCanvas(lv_obj_t* parent, int size,
                        lv_color_t bg, lv_color_t ring,
                        lv_color_t blade, lv_color_t hub) {
    lv_obj_t* cv = lv_canvas_create(parent);
    uint8_t* buf = static_cast<uint8_t*>(malloc(size * size * 2));
    lv_canvas_set_buffer(cv, buf, size, size, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(cv, bg, LV_OPA_COVER);
    lv_obj_set_style_pad_all(cv, 0, 0);
    lv_obj_set_style_border_width(cv, 0, 0);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);

    const float cx = (size - 1) * 0.5f;
    const float cy = (size - 1) * 0.5f;
    const float R  = (size - 1) * 0.5f;
    const float hub_r     = R * 0.16f;
    const float b_in      = hub_r;
    const float b_out     = R - 1.2f;
    const float blade_half = 36.0f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy);
            if (r > R) continue;
            float ang = atan2f(dx, -dy) * 180.0f / 3.14159265f;
            if (ang < 0) ang += 360.0f;
            lv_color_t c;
            bool draw = false;
            if (r <= hub_r) { c = hub; draw = true; }
            else if (r >= R - 1.0f) { c = ring; draw = true; }
            else if (r >= b_in && r <= b_out) {
                float d0 = fmodf(ang, 120.0f);
                float dt = (d0 > 60.0f) ? 120.0f - d0 : d0;
                if (dt <= blade_half) { c = blade; draw = true; }
            }
            if (draw) lv_canvas_set_px(cv, x, y, c, LV_OPA_COVER);
        }
    }
    return cv;
}

// 本地时间 → "HH:MM"，日期 → "YYYY-MM-DD 周X"；时间未同步返回 false
bool GetLocalTime(char* time_buf, size_t time_len,
                  char* date_buf, size_t date_len) {
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) return false;

    std::snprintf(time_buf, time_len, "%02d:%02d", t.tm_hour, t.tm_min);
    if (date_buf && date_len > 0) {
        static const char* kWdays[] = {"周日","周一","周二","周三","周四","周五","周六"};
        std::snprintf(date_buf, date_len, "%04d-%02d-%02d %s",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, kWdays[t.tm_wday % 7]);
    }
    return true;
}

} // namespace

Ili9341TftDisplay::Ili9341TftDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel, int width, int height,
                                     bool mirror_x, bool mirror_y
#if CONFIG_NODE_TOUCH_XPT2046
                                     ,
                                     esp_lcd_touch_handle_t touch_handle
#endif
                                     )
    : panel_io_(panel_io), panel_(panel)
#if CONFIG_NODE_TOUCH_XPT2046
    , touch_handle_(touch_handle)
#endif
{
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

    ESP_LOGI(TAG, "Adding ILI9341 display %dx%d", width, height);
    lvgl_port_display_cfg_t display_cfg = {};
    display_cfg.io_handle = panel_io_;
    display_cfg.panel_handle = panel_;
    display_cfg.buffer_size = static_cast<uint32_t>(width * 20);
    display_cfg.double_buffer = true;
    display_cfg.hres = static_cast<uint32_t>(width);
    display_cfg.vres = static_cast<uint32_t>(height);
    display_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    display_cfg.rotation.swap_xy = false;
    display_cfg.rotation.mirror_x = mirror_x;
    display_cfg.rotation.mirror_y = mirror_y;
    display_cfg.flags.buff_dma = 1;
    display_cfg.flags.swap_bytes = 1;

    lv_display_ = lvgl_port_add_disp(&display_cfg);
    if (lv_display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

#if CONFIG_NODE_TOUCH_XPT2046
    if (touch_handle_ != nullptr) {
        lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_,
            .handle = touch_handle_,
        };
        if (lvgl_port_add_touch(&touch_cfg) != nullptr) {
            ESP_LOGI(TAG, "XPT2046 touch registered");
        } else {
            ESP_LOGW(TAG, "Failed to register touch");
        }
    }
#endif
}

Ili9341TftDisplay::~Ili9341TftDisplay() {
}

bool Ili9341TftDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void Ili9341TftDisplay::Unlock() {
    lvgl_port_unlock();
}

void Ili9341TftDisplay::SetupUI() {
    Display::SetupUI();
#if CONFIG_BOARD_ESP32S3_TFT_FAN
    SetupUI_240x320();
#endif
}

#if CONFIG_BOARD_ESP32S3_TFT_FAN

void Ili9341TftDisplay::SetupUI_240x320() {
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xEAF6FF), 0);

    // ---- 顶部状态栏（240x48，#09254C）----
    lv_obj_t* sb = lv_obj_create(screen);
    lv_obj_set_size(sb, width_, 48);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_style_radius(sb, 0, 0);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x09254C), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* wifi_icon = lv_label_create(sb);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x12D8EF), 0);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, 10, 10);

    conn_dot_ = lv_obj_create(sb);
    lv_obj_set_size(conn_dot_, 7, 7);
    lv_obj_set_style_radius(conn_dot_, 4, 0);
    lv_obj_set_style_bg_color(conn_dot_, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_bg_opa(conn_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(conn_dot_, 0, 0);
    lv_obj_set_pos(conn_dot_, 39, 13);
    lv_obj_clear_flag(conn_dot_, LV_OBJ_FLAG_SCROLLABLE);

    conn_label_ = lv_label_create(sb);
    lv_obj_set_style_text_font(conn_label_, &lv_font_zh14, 0);
    lv_obj_set_style_text_color(conn_label_, lv_color_hex(0xEAF6FF), 0);
    lv_label_set_text(conn_label_, "未连接");
    lv_obj_align(conn_label_, LV_ALIGN_TOP_LEFT, 49, 8);

    wifi_st_label_ = lv_label_create(sb);
    lv_obj_set_style_text_font(wifi_st_label_, &lv_font_zh10, 0);
    lv_obj_set_style_text_color(wifi_st_label_, lv_color_hex(0x8DB9E8), 0);
    lv_label_set_text(wifi_st_label_, "");
    lv_obj_align(wifi_st_label_, LV_ALIGN_TOP_LEFT, 50, 32);

    time_label_ = lv_label_create(sb);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_align(time_label_, LV_ALIGN_TOP_RIGHT, -10, 4);

    date_label_ = lv_label_create(sb);
    lv_obj_set_style_text_font(date_label_, &lv_font_zh10, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0x8DB9E8), 0);
    lv_label_set_text(date_label_, "");
    lv_obj_align(date_label_, LV_ALIGN_TOP_RIGHT, -10, 32);

    lv_obj_t* hline = lv_obj_create(screen);
    lv_obj_set_size(hline, 224, 1);
    lv_obj_set_pos(hline, 8, 47);
    lv_obj_set_style_bg_color(hline, lv_color_hex(0x1768B7), 0);
    lv_obj_set_style_bg_opa(hline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hline, 0, 0);
    lv_obj_set_style_border_width(hline, 0, 0);
    lv_obj_clear_flag(hline, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 标题行：小风扇图标 + 快笼子风扇控制 ----
    lv_obj_t* title_row = lv_obj_create(screen);
    lv_obj_set_size(title_row, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 6, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 0, 50);

    MakeFanCanvas(title_row, 18,
                  lv_color_hex(kColorBg), lv_color_hex(0x12D8EF),
                  lv_color_hex(0x12D8EF), lv_color_hex(0x12D8EF));
    lv_obj_t* title = lv_label_create(title_row);
    lv_obj_set_style_text_font(title, &lv_font_zh16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEAF6FF), 0);
    lv_label_set_text(title, "快笼子风扇控制");

    lv_obj_t* hline2 = lv_obj_create(screen);
    lv_obj_set_size(hline2, 224, 1);
    lv_obj_set_pos(hline2, 8, 75);
    lv_obj_set_style_bg_color(hline2, lv_color_hex(0x143A65), 0);
    lv_obj_set_style_bg_opa(hline2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hline2, 0, 0);
    lv_obj_set_style_border_width(hline2, 0, 0);
    lv_obj_clear_flag(hline2, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 信息卡片（224x91：左湿度 / 右风扇状态）----
    lv_obj_t* data_card = lv_obj_create(screen);
    lv_obj_set_size(data_card, 224, 91);
    lv_obj_set_pos(data_card, 8, 81);
    lv_obj_set_style_radius(data_card, 10, 0);
    lv_obj_set_style_bg_color(data_card, lv_color_hex(0x0B2C59), 0);
    lv_obj_set_style_bg_opa(data_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(data_card, lv_color_hex(0x1768C1), 0);
    lv_obj_set_style_border_width(data_card, 1, 0);
    lv_obj_set_style_pad_all(data_card, 0, 0);
    lv_obj_clear_flag(data_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* water_icon = lv_label_create(data_card);
    lv_obj_set_style_text_font(water_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(water_icon, lv_color_hex(0x12D8EF), 0);
    lv_label_set_text(water_icon, LV_SYMBOL_TINT);
    lv_obj_align(water_icon, LV_ALIGN_TOP_LEFT, 11, 9);

    lv_obj_t* humi_name = lv_label_create(data_card);
    lv_obj_set_style_text_font(humi_name, &lv_font_zh14, 0);
    lv_obj_set_style_text_color(humi_name, lv_color_hex(0xB9D8F7), 0);
    lv_label_set_text(humi_name, "湿度");
    lv_obj_align(humi_name, LV_ALIGN_TOP_LEFT, 39, 11);

    humi_value_label_ = lv_label_create(data_card);
    lv_obj_set_style_text_font(humi_value_label_, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(humi_value_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(humi_value_label_, "--");
    lv_obj_align(humi_value_label_, LV_ALIGN_TOP_LEFT, 35, 25);

    humi_pct_label_ = lv_label_create(data_card);
    lv_obj_set_style_text_font(humi_pct_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(humi_pct_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(humi_pct_label_, "%");
    lv_obj_align_to(humi_pct_label_, humi_value_label_, LV_ALIGN_OUT_RIGHT_MID, 2, 4);

    MakeStatusTag(data_card, "正常", 0x20E3AA, &humi_status_label_);
    lv_obj_align(lv_obj_get_parent(humi_status_label_), LV_ALIGN_TOP_LEFT, 35, 67);

    lv_obj_t* vline = lv_obj_create(data_card);
    lv_obj_set_size(vline, 1, 73);
    lv_obj_set_style_bg_color(vline, lv_color_hex(0x1768C1), 0);
    lv_obj_set_style_bg_opa(vline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vline, 0, 0);
    lv_obj_set_style_border_width(vline, 0, 0);
    lv_obj_align(vline, LV_ALIGN_TOP_LEFT, 112, 9);
    lv_obj_clear_flag(vline, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* st_row = lv_obj_create(data_card);
    lv_obj_set_size(st_row, 104, 22);
    lv_obj_set_style_bg_opa(st_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st_row, 0, 0);
    lv_obj_set_style_pad_all(st_row, 0, 0);
    lv_obj_set_style_pad_column(st_row, 4, 0);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(st_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(st_row, LV_ALIGN_TOP_LEFT, 114, 30);

    MakeFanCanvas(st_row, 20,
                  lv_color_hex(0x0B2C59), lv_color_hex(0x1C86E5),
                  lv_color_hex(0x12D8EF), lv_color_hex(0xFFFFFF));
    lv_obj_t* st_title = lv_label_create(st_row);
    lv_obj_set_style_text_font(st_title, &lv_font_zh14, 0);
    lv_obj_set_style_text_color(st_title, lv_color_hex(0xB9D8F7), 0);
    lv_label_set_text(st_title, "风扇状态");

    fan_state_label_ = lv_label_create(data_card);
    lv_obj_set_style_text_font(fan_state_label_, &lv_font_zh16, 0);
    lv_obj_set_style_text_color(fan_state_label_, lv_color_hex(0x90A0B0), 0);
    lv_label_set_text(fan_state_label_, "已停止");
    lv_obj_set_width(fan_state_label_, 104);
    lv_obj_set_style_text_align(fan_state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fan_state_label_, LV_ALIGN_TOP_LEFT, 114, 56);

    // ---- 底部风扇开关大按钮（224x130，触摸切换）----
    btn_fan_ = lv_button_create(screen);
    lv_obj_set_pos(btn_fan_, 8, 181);
    lv_obj_set_size(btn_fan_, 224, 130);
    lv_obj_set_style_radius(btn_fan_, 14, 0);
    lv_obj_set_style_bg_color(btn_fan_, lv_color_hex(kColorBtnOff), 0);
    lv_obj_set_style_bg_opa(btn_fan_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_fan_, lv_color_hex(0x48617F), 0);
    lv_obj_set_style_border_width(btn_fan_, 2, 0);
    lv_obj_set_style_pad_all(btn_fan_, 0, 0);
    lv_obj_set_style_shadow_width(btn_fan_, 0, 0);
    lv_obj_clear_flag(btn_fan_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn_fan_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(btn_fan_, &Ili9341TftDisplay::FanButtonEventHandler,
                        LV_EVENT_CLICKED, this);

    lv_obj_t* power_icon = lv_label_create(btn_fan_);
    lv_obj_set_style_text_font(power_icon, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(power_icon, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(power_icon, LV_SYMBOL_POWER);
    lv_obj_align(power_icon, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t* fan_title = lv_label_create(btn_fan_);
    lv_obj_set_style_text_font(fan_title, &lv_font_zh24, 0);
    lv_obj_set_style_text_color(fan_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(fan_title, "风扇开关");
    lv_obj_align(fan_title, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t* hint = lv_label_create(btn_fan_);
    lv_obj_set_style_text_font(hint, &lv_font_zh14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB9D8F7), 0);
    lv_label_set_text(hint, "点击控制风扇");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 92);

    RefreshFanButton();

    // 状态栏时间/日期 1s 任务（与传感器无关）
    BaseType_t ret = xTaskCreate(&Ili9341TftDisplay::StatusTask, "fan-status",
                                 2048, this, 3, &status_task_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create status task failed, free heap %u B",
                 static_cast<unsigned>(esp_get_free_heap_size()));
    }
}

void Ili9341TftDisplay::SetStatus(const char* status) {
    if (conn_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    lv_label_set_text(conn_label_, status);
    // 状态圆点：已连接=绿，其他=橙
    if (conn_dot_ != nullptr) {
        bool connected = std::strcmp(status, "已连接") == 0;
        lv_obj_set_style_bg_color(conn_dot_,
                                  lv_color_hex(connected ? 0x19D9A0 : 0xF59E0B), 0);
    }
}

void Ili9341TftDisplay::SetFanToggleCallback(std::function<void(bool)> callback) {
    fan_callback_ = std::move(callback);
}

void Ili9341TftDisplay::UpdateEnv(float temperature_c, float humidity_pct) {
    (void)temperature_c;  // V2 布局无温度显示位
    if (humi_value_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }

    char text[16];

    if (std::isfinite(humidity_pct)) {
        std::snprintf(text, sizeof(text), "%.0f", static_cast<double>(humidity_pct));
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    if (std::strcmp(humi_cache_, text) != 0) {
        std::strncpy(humi_cache_, text, sizeof(humi_cache_) - 1);
        lv_label_set_text(humi_value_label_, humi_cache_);
        // 数值宽度变化后重新跟随
        lv_obj_align_to(humi_pct_label_, humi_value_label_,
                        LV_ALIGN_OUT_RIGHT_MID, 2, 4);
    }

    if (humi_status_label_ != nullptr) {
        if (std::isfinite(humidity_pct)) {
            int pct = static_cast<int>(humidity_pct + 0.5f);
            pct = std::clamp(pct, 0, 100);
            lv_label_set_text(humi_status_label_, pct > 70 ? "偏高" : "正常");
        } else {
            lv_label_set_text(humi_status_label_, "--");
        }
    }
}

void Ili9341TftDisplay::FanButtonEventHandler(lv_event_t* event) {
    auto* self = static_cast<Ili9341TftDisplay*>(lv_event_get_user_data(event));
    if (self != nullptr) {
        self->ToggleFan();
    }
}

void Ili9341TftDisplay::ToggleFan() {
    fan_running_ = !fan_running_;
    if (fan_callback_) {
        fan_callback_(fan_running_);
    }
    RefreshFanButton();
}

void Ili9341TftDisplay::SetFanRunning(bool running) {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    fan_running_ = running;
    RefreshFanButton();
}

void Ili9341TftDisplay::RefreshFanButton() {
    if (btn_fan_ != nullptr) {
        lv_obj_set_style_bg_color(btn_fan_,
                                  lv_color_hex(fan_running_ ? kColorBtnOn : kColorBtnOff), 0);
    }
    if (fan_state_label_ != nullptr) {
        lv_label_set_text(fan_state_label_, fan_running_ ? "运行中" : "已停止");
        lv_obj_set_style_text_color(fan_state_label_,
                                    lv_color_hex(fan_running_ ? 0x00FF88 : 0x90A0B0), 0);
    }
}

void Ili9341TftDisplay::StatusTask(void* arg) {
    auto* self = static_cast<Ili9341TftDisplay*>(arg);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (self->time_label_ == nullptr) {
            continue;
        }

        DisplayLockGuard lock(self);
        if (!lock) {
            continue;
        }

        char tbuf[8], dbuf[24];
        if (GetLocalTime(tbuf, sizeof(tbuf), dbuf, sizeof(dbuf))) {
            lv_label_set_text(self->time_label_, tbuf);
            if (self->date_label_ != nullptr) {
                lv_label_set_text(self->date_label_, dbuf);
            }
        } else {
            lv_label_set_text(self->time_label_, "--:--");
            if (self->date_label_ != nullptr) {
                lv_label_set_text(self->date_label_, "");
            }
        }
    }
}

#endif // CONFIG_BOARD_ESP32S3_TFT_FAN
