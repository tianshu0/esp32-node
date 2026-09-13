#include "display_service/DashboardScreen.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "dashboard";

// 从 values_json 取一个数值字段，如 ExtractFloat("{\"temp\":23.5}", "temp", &v)
static bool ExtractFloat(const char* json, const char* key, float* out)
{
    if (json == nullptr || key == nullptr || out == nullptr) {
        return false;
    }
    char pattern[24];
    std::snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = std::strstr(json, pattern);
    if (p == nullptr) {
        return false;
    }
    p += std::strlen(pattern);
    while (*p == ' ') {
        ++p;
    }
    if (*p != ':') {
        return false;
    }
    ++p;
    while (*p == ' ') {
        ++p;
    }

    char* end = nullptr;
    float value = std::strtof(p, &end);
    if (end == p) {
        return false;
    }
    *out = value;
    return true;
}

// 文本变化才写回 label，省掉一次整屏 8KB 的 I2C 刷新
static void UpdateLabel(lv_obj_t* label, char* cache, size_t cap, const char* text)
{
    if (label == nullptr || cache == nullptr || text == nullptr) {
        return;
    }
    if (std::strcmp(cache, text) == 0) {
        return;
    }
    std::strncpy(cache, text, cap - 1);
    cache[cap - 1] = '\0';
    lv_label_set_text(label, cache);
}

void DashboardScreen::Build(lv_obj_t* scr, const char* node_id,
                            const SensorRegistry& registry)
{
    // esp_lvgl_port 的 I1 转换把 LVGL 白(bit=1)映射为 GDDRAM 0(灭)、
    // LVGL 黑(bit=0)映射为 GDDRAM 1(亮)。因此 LVGL 侧按「白底黑字」绘制，
    // 面板上呈现的才是「黑底亮字」。
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // ---- 页眉：node-id（左）+ 连接状态（右）----
    lv_obj_t* node_label = lv_label_create(scr);
    lv_obj_set_style_text_font(node_label, &lv_font_montserrat_10, 0);
    lv_label_set_text(node_label, node_id != nullptr ? node_id : "");
    lv_obj_set_pos(node_label, 2, 1);

    status_label_ = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_10, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, -2, 1);

    lv_obj_t* divider = lv_obj_create(scr);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, kWidth, 1);
    lv_obj_set_pos(divider, 0, 13);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    // ---- 先统计字段总数，用于按行数选字号/行距（分隔线下 14..63 共 50px）----
    int total = 0;
    for (int s = 0; s < registry.Count(); ++s) {
        total += registry.FieldCountAt(s);
    }

    const lv_font_t* tag_font = &lv_font_montserrat_12;
    const lv_font_t* val_font = &lv_font_montserrat_14;
    int y0 = 15;
    int pitch = 16;
    if (total >= 5) {
        tag_font = val_font = &lv_font_montserrat_10;
        y0 = 14;
        pitch = 9;
    } else if (total == 4) {
        tag_font = &lv_font_montserrat_10;
        val_font = &lv_font_montserrat_12;
        y0 = 14;
        pitch = 12;
    }

    // ---- 依据注册表字段描述符自动展开行 ----
    row_count_ = 0;
    for (int s = 0; s < registry.Count(); ++s) {
        for (int f = 0; f < registry.FieldCountAt(s); ++f) {
            if (row_count_ >= kMaxRows) {
                ESP_LOGW(TAG, "too many fields, rest not shown");
                goto built;
            }
            const SensorField& fld = registry.FieldAt(s, f);
            Row& row = rows_[row_count_];
            std::strncpy(row.sensor_type, registry.TypeAt(s), sizeof(row.sensor_type) - 1);
            std::strncpy(row.key, fld.key, sizeof(row.key) - 1);
            std::strncpy(row.unit, fld.unit, sizeof(row.unit) - 1);
            row.decimals = fld.decimals;

            int y = y0 + row_count_ * pitch;
            lv_obj_t* tag_label = lv_label_create(scr);
            lv_obj_set_style_text_font(tag_label, tag_font, 0);
            lv_label_set_text(tag_label, fld.label);
            lv_obj_set_pos(tag_label, 2, y + (val_font == &lv_font_montserrat_14 ? 2 : 0));

            lv_obj_t* value_label = lv_label_create(scr);
            lv_obj_set_style_text_font(value_label, val_font, 0);
            lv_obj_set_style_text_letter_space(value_label, 1, 0);
            lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -2, y);
            row.value = value_label;

            char placeholder[20];
            std::snprintf(placeholder, sizeof(placeholder), "-- %s", fld.unit);
            UpdateLabel(value_label, row.cache, sizeof(row.cache), placeholder);
            ++row_count_;
        }
    }
built:

    UpdateLabel(status_label_, status_cache_, sizeof(status_cache_), "ADV");
}

void DashboardScreen::SetStatus(const char* status)
{
    UpdateLabel(status_label_, status_cache_, sizeof(status_cache_),
                status != nullptr ? status : "");
}

void DashboardScreen::Update(const SensorReading* samples, int count,
                             const SensorRegistry& /*registry*/)
{
    for (int i = 0; i < row_count_; ++i) {
        Row& row = rows_[i];
        if (row.value == nullptr) {
            continue;
        }

        float value = 0.0f;
        bool ok = false;
        for (int j = 0; j < count && samples != nullptr; ++j) {
            if (std::strncmp(samples[j].type, row.sensor_type,
                             sizeof(samples[j].type)) == 0 &&
                ExtractFloat(samples[j].values_json, row.key, &value)) {
                ok = true;
                break;
            }
        }

        char text[20];
        if (ok) {
            std::snprintf(text, sizeof(text), "%.*f %s",
                          static_cast<int>(row.decimals),
                          static_cast<double>(value), row.unit);
        } else {
            std::snprintf(text, sizeof(text), "-- %s", row.unit);
        }
        UpdateLabel(row.value, row.cache, sizeof(row.cache), text);
    }
}

} // namespace esp32node
