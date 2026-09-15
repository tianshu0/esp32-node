#include "display_service/CompactDashboardScreen.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"

namespace esp32node {

static const char* TAG = "compact_dash";

// 从 values_json 取一个数值字段
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
    while (*p == ' ') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ') ++p;

    char* end = nullptr;
    float value = std::strtof(p, &end);
    if (end == p) return false;
    *out = value;
    return true;
}

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

void CompactDashboardScreen::Build(lv_obj_t* scr, const char* node_id,
                                   const SensorRegistry& registry)
{
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // ---- 页眉：node-id（左）+ 连接状态（右），montserrat_10 约 11px ----
    lv_obj_t* node_label = lv_label_create(scr);
    lv_obj_set_style_text_font(node_label, &lv_font_montserrat_10, 0);
    lv_label_set_text(node_label, node_id != nullptr ? node_id : "");
    lv_obj_set_pos(node_label, 2, 1);

    status_label_ = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_10, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, -2, 1);

    // 统计总字段数
    total_fields_ = 0;
    for (int s = 0; s < registry.Count(); ++s) {
        total_fields_ += registry.FieldCountAt(s);
    }

    // ---- 2x2 数据格：y=13 起，行距 10px（montserrat_10）----
    static const int kRowY[2] = {13, 23};
    for (int i = 0; i < kCells; ++i) {
        int row = i / 2;
        int col = i % 2;
        cells_[i].label = lv_label_create(scr);
        lv_obj_set_style_text_font(cells_[i].label, &lv_font_montserrat_10, 0);
        if (col == 0) {
            lv_obj_set_pos(cells_[i].label, 2, kRowY[row]);
        } else {
            lv_obj_align(cells_[i].label, LV_ALIGN_TOP_RIGHT, -2, kRowY[row]);
        }
        UpdateLabel(cells_[i].label, cells_[i].cache, sizeof(cells_[i].cache), "--");
    }

    page_ = 0;
    last_page_switch_ms_ = 0;
    UpdateLabel(status_label_, status_cache_, sizeof(status_cache_), "ADV");
}

void CompactDashboardScreen::SetStatus(const char* status)
{
    UpdateLabel(status_label_, status_cache_, sizeof(status_cache_),
                status != nullptr ? status : "");
}

void CompactDashboardScreen::Update(const SensorReading* samples, int count,
                                    const SensorRegistry& registry)
{
    if (total_fields_ == 0) {
        return;
    }

    // 多页时按周期切换起始字段
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (total_fields_ > kCells) {
        if (last_page_switch_ms_ == 0) {
            last_page_switch_ms_ = now;
        } else if (now - last_page_switch_ms_ >= kPageMs) {
            page_ = (page_ + 1) % total_fields_;
            last_page_switch_ms_ = now;
        }
    }

    // 把 registry 的所有字段展平为线性列表，按 page_ 偏移取 kCells 个
    for (int i = 0; i < kCells; ++i) {
        Cell& cell = cells_[i];
        if (cell.label == nullptr) {
            continue;
        }

        int field_idx = (page_ + i) % total_fields_;

        // 定位第 field_idx 个字段属于哪个传感器的第几个字段
        int s = 0;
        int f = field_idx;
        for (; s < registry.Count(); ++s) {
            int fc = registry.FieldCountAt(s);
            if (f < fc) {
                break;
            }
            f -= fc;
        }
        if (s >= registry.Count()) {
            UpdateLabel(cell.label, cell.cache, sizeof(cell.cache), "--");
            continue;
        }

        const SensorField& fld = registry.FieldAt(s, f);
        const char* stype = registry.TypeAt(s);
        std::strncpy(cell.sensor_type, stype, sizeof(cell.sensor_type) - 1);
        std::strncpy(cell.key, fld.key, sizeof(cell.key) - 1);
        std::strncpy(cell.unit, fld.unit, sizeof(cell.unit) - 1);
        cell.decimals = fld.decimals;

        // 从本周期采样中找匹配值
        float value = 0.0f;
        bool ok = false;
        for (int j = 0; j < count && samples != nullptr; ++j) {
            if (std::strncmp(samples[j].type, cell.sensor_type,
                             sizeof(samples[j].type)) == 0 &&
                ExtractFloat(samples[j].values_json, cell.key, &value)) {
                ok = true;
                break;
            }
        }

        char text[24];
        if (ok) {
            std::snprintf(text, sizeof(text), "%s %.*f%s",
                          fld.label,
                          static_cast<int>(cell.decimals),
                          static_cast<double>(value),
                          cell.unit);
        } else {
            std::snprintf(text, sizeof(text), "%s --%s", fld.label, cell.unit);
        }
        UpdateLabel(cell.label, cell.cache, sizeof(cell.cache), text);
    }
}

} // namespace esp32node
