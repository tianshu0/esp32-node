// ThpDisplay 实现：SSD1315 面板 + 温湿度气压三行布局
//
// 传感器 type 写死为 "sht3x" / "bmp180"（对应 Sht3xSensor / Bmp180Sensor 的注册名）
// UI 布局：页眉(node-id + 状态) + 分隔线 + 3 行（温度/湿度/气压）
#include "thp_display.hpp"
#include "esp_lcd_panel_ssd1306.h"
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"

namespace esp32node {

static const char* TAG = "thp-display";

// 面板 IO 的时钟（与板装配层建 I2C 总线时给从设备的一致）
static constexpr uint32_t kPanelI2cClkHz = 400000;

ThpDisplay::ThpDisplay(i2c_master_bus_handle_t bus, uint8_t addr, int w, int h, bool mirror_x, bool mirror_y)
    : bus_(bus), addr_(addr), mirror_x_(mirror_x), mirror_y_(mirror_y)
{
    // 注意：width_/height_ 仅在全部初始化成功后才赋值，
    // Board 以 width()>0 判断显示是否可用。
    if (!bus_) {
        ESP_LOGE(TAG, "I2C bus handle is null");
        return;
    }

    // ---------- 创建 I2C panel IO ----------
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = addr_;
    io_cfg.scl_speed_hz = kPanelI2cClkHz;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 6;  // SSD1306/SSD1315 D/C# 在控制字节 bit6
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;

    esp_err_t err = esp_lcd_new_panel_io_i2c(bus_, &io_cfg, &io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io failed: %s", esp_err_to_name(err));
        return;
    }

    // ---------- 创建 SSD1315 面板 ----------
    esp_lcd_panel_ssd1306_config_t ssd_cfg = {};
    ssd_cfg.height = h;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = -1;
    panel_cfg.bits_per_pixel = 1;
    panel_cfg.vendor_config = &ssd_cfg;

    err = esp_lcd_new_panel_ssd1306(io_, &panel_cfg, &panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel ssd1306 failed: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_reset(panel_);
    esp_lcd_panel_init(panel_);
    esp_lcd_panel_disp_on_off(panel_, true);

    // 硬件镜像（旋转 180°）：排针侧朝上安装
    if (mirror_x_ || mirror_y_) {
        esp_lcd_panel_mirror(panel_, mirror_x_, mirror_y_);
    }

    // ---------- 挂 LVGL ----------
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = io_;
    disp_cfg.panel_handle = panel_;
    disp_cfg.buffer_size = w * h;  // 单色屏整屏缓冲
    disp_cfg.double_buffer = false;
    disp_cfg.hres = w;
    disp_cfg.vres = h;
    disp_cfg.monochrome = true;
    disp_cfg.color_format = LV_COLOR_FORMAT_I1;
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma = false;
    disp_cfg.flags.swap_bytes = false;

    disp_ = lvgl_port_add_disp(&disp_cfg);
    if (!disp_) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return;
    }

    // 全部成功后才标记分辨率（Board 用 width()>0 判断成功）
    width_ = w;
    height_ = h;
    ESP_LOGI(TAG, "thp display ready: %dx%d addr=0x%02X", width_, height_, addr_);
}

bool ThpDisplay::Lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void ThpDisplay::Unlock()
{
    lvgl_port_unlock();
}

void ThpDisplay::BuildUi(const char* node_id)
{
    DisplayLockGuard guard(this);
    if (!guard) {
        return;
    }

    lv_obj_t* scr = lv_display_get_screen_active(disp_);

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
    lv_label_set_text(node_label, node_id ? node_id : "");
    lv_obj_set_pos(node_label, 2, 1);

    status_label_ = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_10, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, -2, 1);

    lv_obj_t* divider = lv_obj_create(scr);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, width_, 1);
    lv_obj_set_pos(divider, 0, 13);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    // ---- 三行：温度/湿度/气压（固定行高 16px，y=15/31/47）----
    static const struct {
        const char* label;
        const char* unit;
        uint8_t decimals;
    } kRows[] = {
        {"Temp", "C", 1},
        {"Humi", "%", 1},
        {"Pres", "hPa", 1},
    };

    for (int i = 0; i < 3; ++i) {
        int y = 15 + i * 16;

        lv_obj_t* tag = lv_label_create(scr);
        lv_obj_set_style_text_font(tag, &lv_font_montserrat_14, 0);
        lv_label_set_text(tag, kRows[i].label);
        lv_obj_set_pos(tag, 2, y + 2);

        lv_obj_t* value = lv_label_create(scr);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(value, 1, 0);
        lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -2, y);
        rows_[i].value = value;

        char placeholder[20];
        std::snprintf(placeholder, sizeof(placeholder), "-- %s", kRows[i].unit);
        lv_label_set_text(value, placeholder);
        std::strncpy(rows_[i].cache, placeholder, sizeof(rows_[i].cache) - 1);
    }

    lv_label_set_text(status_label_, "ADV");
    std::strncpy(status_cache_, "ADV", sizeof(status_cache_) - 1);
}

void ThpDisplay::SetStatus(const char* status)
{
    if (!status_label_) return;
    DisplayLockGuard guard(this);
    if (!guard) return;

    if (std::strcmp(status_cache_, status) == 0) return;
    std::strncpy(status_cache_, status, sizeof(status_cache_) - 1);
    status_cache_[sizeof(status_cache_) - 1] = '\0';
    lv_label_set_text(status_label_, status_cache_);
}

void ThpDisplay::UpdateSamples(const SensorReading* samples, int count)
{
    if (!samples || count <= 0) return;
    DisplayLockGuard guard(this);
    if (!guard) return;
    UpdateRows(samples, count);
}

void ThpDisplay::UpdateRows(const SensorReading* samples, int count)
{
    // 传感器 type 与字段 key 映射（对应 Sht3xSensor / Bmp180Sensor 的注册名）
    static const struct {
        const char* sensor_type;
        const char* key;
        const char* unit;
        uint8_t decimals;
    } kMap[] = {
        {"sht3x", "temperature", "C", 1},
        {"sht3x", "humidity", "%", 1},
        {"bmp180", "pressure", "hPa", 1},
    };

    for (int i = 0; i < 3; ++i) {
        Row& row = rows_[i];
        if (!row.value) continue;

        float value = 0.0f;
        bool ok = false;
        for (int j = 0; j < count; ++j) {
            if (std::strncmp(samples[j].type, kMap[i].sensor_type,
                             sizeof(samples[j].type)) == 0) {
                // 简单提取：{"temperature":23.5} 格式
                char pattern[24];
                std::snprintf(pattern, sizeof(pattern), "\"%s\":", kMap[i].key);
                const char* p = std::strstr(samples[j].values_json, pattern);
                if (p) {
                    p += std::strlen(pattern);
                    char* end = nullptr;
                    value = std::strtof(p, &end);
                    ok = (end != p);
                }
                if (ok) break;
            }
        }

        char text[20];
        if (ok) {
            std::snprintf(text, sizeof(text), "%.*f %s",
                          static_cast<int>(kMap[i].decimals),
                          static_cast<double>(value), kMap[i].unit);
        } else {
            std::snprintf(text, sizeof(text), "-- %s", kMap[i].unit);
        }

        if (std::strcmp(row.cache, text) != 0) {
            std::strncpy(row.cache, text, sizeof(row.cache) - 1);
            row.cache[sizeof(row.cache) - 1] = '\0';
            lv_label_set_text(row.value, row.cache);
        }
    }
}

} // namespace esp32node
