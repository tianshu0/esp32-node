#include "oled_display/OledDisplay.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config/AppConfig.hpp"
#include "ble_peripheral/BlePeripheral.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "sensor_registry/SensorRegistry.hpp"

#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"

namespace esp32node {

static const char* TAG = "oled_display";

// 从 values_json 里取一个数值字段，如 ExtractFloat("{\"temp\":23.5}", "temp", &v)
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

// 文本变化才写回 label：内容未变时不置脏，省掉一次整屏 8KB 的 I2C 刷新
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

// 主体一行的「标签 + 右对齐数值」；返回数值 label 供刷新。
// 标签 12px、数值 14px（line_height=16），数值加 1px 字距：
// 单色 128x64 屏上小字发虚，放大字号 + 拉开字距比挤在一起清晰。
static lv_obj_t* MakeRow(lv_obj_t* scr, const char* tag, int y)
{
    lv_obj_t* tag_label = lv_label_create(scr);
    lv_obj_set_style_text_font(tag_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(tag_label, tag);
    lv_obj_set_pos(tag_label, 2, y + 2);

    lv_obj_t* value_label = lv_label_create(scr);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(value_label, 1, 0);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -2, y);
    return value_label;
}

OledDisplay::~OledDisplay()
{
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    // 面板与 LVGL 显示由 esp_lvgl_port 管理，进程重启时无需释放
}

esp_err_t OledDisplay::Init(AppConfig* config, SensorRegistry* registry,
                            BlePeripheral* ble, I2cBus* bus)
{
    if (config == nullptr || registry == nullptr || ble == nullptr || bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    registry_ = registry;
    ble_ = ble;
    bus_ = bus;

    esp_err_t err = SetupPanel();
    if (err != ESP_OK) {
        return err;
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = io_;
    disp_cfg.panel_handle = panel_;
    disp_cfg.buffer_size = kWidth * kHeight;  // 单色屏必须整屏缓冲，否则 lvgl_port 拒绝
    disp_cfg.double_buffer = false;           // 1s 刷新一次，双缓冲只会白占 8KB
    disp_cfg.hres = kWidth;
    disp_cfg.vres = kHeight;
    disp_cfg.monochrome = true;
    disp_cfg.color_format = LV_COLOR_FORMAT_I1;
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma = false;
    disp_cfg.flags.swap_bytes = false;
    disp_ = lvgl_port_add_disp(&disp_cfg);
    if (disp_ == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_FAIL;
    }

    if (lvgl_port_lock(0)) {
        BuildUi();
        lvgl_port_unlock();
    }

    BaseType_t ok = xTaskCreate(TaskThunk, "oled_display", kTaskStack, this,
                                kTaskPriority, &task_);
    if (ok != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "xTaskCreate failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "oled ready: %dx%d addr=0x%02X refresh=%lums free heap=%u",
             kWidth, kHeight, addr_, static_cast<unsigned long>(kRefreshMs),
             static_cast<unsigned>(esp_get_free_heap_size()));
    return ESP_OK;
}

esp_err_t OledDisplay::SetupPanel()
{
    // 模块地址 0x3C 为主，部分板子是 0x3D
    if (bus_->Probe(kAddrPrimary)) {
        addr_ = kAddrPrimary;
    } else if (bus_->Probe(kAddrAlt)) {
        addr_ = kAddrAlt;
    } else {
        ESP_LOGW(TAG, "oled not found at 0x3C/0x3D, check wiring");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = addr_;
    io_cfg.scl_speed_hz = I2cBus::kDefaultClkHz;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 6;  // SSD1306/SSD1315 的 D/C# 在控制字节 bit6
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus_->Handle(), &io_cfg, &io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new panel io failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_ssd1306_config_t ssd_cfg = {};
    ssd_cfg.height = kHeight;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = -1;  // 模块无复位脚
    panel_cfg.bits_per_pixel = 1;
    panel_cfg.vendor_config = &ssd_cfg;
    err = esp_lcd_new_panel_ssd1306(io_, &panel_cfg, &panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new panel ssd1306 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(panel_);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 安装方向以排针侧为上，需要相对当前画面再转 180°：X/Y 同时镜像。
    // 当前 COM 已镜像（0xC8，修正模块上下颠倒），再打开 SEG 重映射（0xA1）
    // 即 0xA1 + 0xC8 = 旋转 180°。纯硬件扫描方向处理，无软件旋转开销。
    // 不能只改 disp_cfg.rotation.mirror_*：端口只在收到 LVGL 旋转事件时才把
    // mirror 下发给面板，而 ROTATION_0 -> ROTATION_0 不产生事件。
    err = esp_lcd_panel_mirror(panel_, true, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel mirror failed: %s", esp_err_to_name(err));
    }
    return err;
}

void OledDisplay::BuildUi()
{
    // esp_lvgl_port 的 I1 转换把 LVGL 白(bit=1)映射为 GDDRAM 0(灭)、
    // LVGL 黑(bit=0)映射为 GDDRAM 1(亮)。因此 LVGL 侧必须按「白底黑字」绘制，
    // 面板上呈现的才是想要的「黑底亮字」；若画成黑底白字会整屏反色。
    lv_obj_t* scr = lv_display_get_screen_active(disp_);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t* node_label = lv_label_create(scr);
    lv_obj_set_style_text_font(node_label, &lv_font_montserrat_10, 0);
    lv_label_set_text(node_label, config_->NodeId().c_str());
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

    // 分隔线下 14..63 共 50px，三行 14 号字（行高 16）按 16px 行距排 y=15/31/47，
    // 最后一行行框到 63，恰好占满 64px 且不裁切。
    temp_label_ = MakeRow(scr, "T", 15);
    hum_label_ = MakeRow(scr, "H", 31);
    press_label_ = MakeRow(scr, "P", 47);

    UpdateLabel(status_label_, last_status_, sizeof(last_status_), StatusText());
    UpdateLabel(temp_label_, last_temp_, sizeof(last_temp_), "-- \xC2\xB0" "C");
    UpdateLabel(hum_label_, last_hum_, sizeof(last_hum_), "-- %");
    UpdateLabel(press_label_, last_press_, sizeof(last_press_), "-- hPa");
}

OledDisplay::Reading OledDisplay::ReadSensors() const
{
    Reading r;
    SensorReading readings[SensorRegistry::kMaxSensors] = {};

    // 与 data_pipeline 的采集互斥，避免一次「命令 + 等待 + 读回」被穿插
    bus_->Lock();
    int n = registry_->ReadAll(readings, SensorRegistry::kMaxSensors);
    bus_->Unlock();

    for (int i = 0; i < n; ++i) {
        if (std::strcmp(readings[i].type, "temp_hum") == 0) {
            r.temp_ok = ExtractFloat(readings[i].values_json, "temp", &r.temp_c);
            r.hum_ok = ExtractFloat(readings[i].values_json, "humidity", &r.humidity);
        } else if (std::strcmp(readings[i].type, "pressure") == 0) {
            r.press_ok = ExtractFloat(readings[i].values_json, "pressure", &r.pressure_hpa);
        }
    }
    return r;
}

const char* OledDisplay::StatusText() const
{
    if (ble_->IsPaired()) {
        return "PAIRED";
    }
    if (ble_->IsConnected()) {
        return "CONN";
    }
    return "ADV";
}

void OledDisplay::TaskThunk(void* ctx)
{
    static_cast<OledDisplay*>(ctx)->Run();
}

void OledDisplay::Run()
{
    char buf[20];
    while (true) {
        Reading r = ReadSensors();
        const char* status = StatusText();

        if (lvgl_port_lock(0)) {
            UpdateLabel(status_label_, last_status_, sizeof(last_status_), status);

            if (r.temp_ok) {
                std::snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C", r.temp_c);
            } else {
                std::snprintf(buf, sizeof(buf), "-- \xC2\xB0" "C");
            }
            UpdateLabel(temp_label_, last_temp_, sizeof(last_temp_), buf);

            if (r.hum_ok) {
                std::snprintf(buf, sizeof(buf), "%.1f %%", r.humidity);
            } else {
                std::snprintf(buf, sizeof(buf), "-- %%");
            }
            UpdateLabel(hum_label_, last_hum_, sizeof(last_hum_), buf);

            if (r.press_ok) {
                std::snprintf(buf, sizeof(buf), "%.1f hPa", r.pressure_hpa);
            } else {
                std::snprintf(buf, sizeof(buf), "-- hPa");
            }
            UpdateLabel(press_label_, last_press_, sizeof(last_press_), buf);

            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(kRefreshMs));
    }
}

} // namespace esp32node
