// SpitftTouchDisplay 实现：SPI TFT + XPT2046 触摸 + LVGL 9 多页 UI
//
// 初始化顺序（必须遵守）：
//   1. Board.cpp 先初始化 SPI2_HOST 总线（spi_bus_initialize）
//   2. Configure() 传入引脚和风扇指针
//   3. Start() 内依次：InitLcd → InitTouch → InitLvgl → BuildAllPages
//   4. RefreshTask 每秒更新传感器数据到 Dashboard
//
// 适配 LVGL 9.6：
//   - 输入设备用 lv_indev_create()/lv_indev_set_type()/lv_indev_set_read_cb()
//     （旧的 lv_indev_drv_register 在 LVGL9 已删除）
//   - 关闭滚动用 lv_obj_set_scrollable(obj, false)（旧 API 已 deprecated）
//   - 所有 LVGL 对象操作必须在 lvgl_port_lock 内
#include "display_service/SpitftTouchDisplay.hpp"
#include "display_service/esp_lcd_ili9341.h"
#include "display_service/display_fonts.h"
#if CONFIG_NODE_TOUCH_XPT2046
#include "display_service/xpt2046.h"
#endif
#include "fan_control/FanControl.hpp"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "sensor_registry/SensorRegistry.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace esp32node {

static const char* TAG = "spi-tft";

// 中文字体（SimHei 子集，含 ASCII；新增汉字需用 lv_font_conv 重新生成）
static inline const lv_font_t* Font16() { return &lv_font_zh16; }
static inline const lv_font_t* Font24() { return &lv_font_zh24; }

esp_err_t SpitftTouchDisplay::Start(const DisplayContext& ctx)
{
    ctx_ = ctx;

    ESP_RETURN_ON_ERROR(InitLcd(),   TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(InitTouch(), TAG, "Touch init failed");
    ESP_RETURN_ON_ERROR(InitLvgl(),  TAG, "LVGL init failed");

    // 所有 LVGL 对象创建必须在锁内
    lvgl_port_lock(0);
    BuildAllPages();
    lv_screen_load(scr_dashboard_);
    lvgl_port_unlock();

    if (xTaskCreate(&SpitftTouchDisplay::RefreshTask, "tft-refresh", 4096,
                    this, 3, &task_) != pdPASS) {
        ESP_LOGE(TAG, "create refresh task failed, free heap %u B",
                 (unsigned)esp_get_free_heap_size());
    }

    ESP_LOGI(TAG, "SPI TFT + Touch init ok (%dx%d)", cfg_.screen_w, cfg_.screen_h);
    return ESP_OK;
}

// ==================== LCD ====================

esp_err_t SpitftTouchDisplay::InitLcd()
{
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = pins_.lcd_cs;
    io_cfg.dc_gpio_num = pins_.lcd_dc;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = cfg_.lcd_pclk_mhz * 1000 * 1000;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.trans_queue_depth = 10;

    esp_err_t err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)pins_.spi_host, &io_cfg, &lcd_io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = pins_.lcd_rst;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR; // ILI9341 默认 BGR
    panel_cfg.bits_per_pixel = 16;

    err = esp_lcd_new_panel_ili9341(lcd_io_, &panel_cfg, &lcd_panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_ili9341 failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_reset(lcd_panel_);
    esp_lcd_panel_init(lcd_panel_);
    esp_lcd_panel_set_gap(lcd_panel_, 0, 0);

    // 方向（必须与 disp_cfg.rotation 完全一致，lvgl_port_add_disp 会重发 MADCTL）：
    //   竖屏排针朝下：实测仅需 MX（BGR|MX=0x48）修正左右镜像；
    //   0x08 左右反，0xC8 上下左右全反（旋转180°）。
    //   横屏排针朝左：MV(0x28)。
    if (cfg_.rotation == ScreenRotation::Portrait) {
        esp_lcd_panel_swap_xy(lcd_panel_, false);
        esp_lcd_panel_mirror(lcd_panel_, true, false);
    } else {
        esp_lcd_panel_swap_xy(lcd_panel_, true);
        esp_lcd_panel_mirror(lcd_panel_, false, false);
    }
    esp_lcd_panel_disp_on_off(lcd_panel_, true);

    if (pins_.lcd_bl >= 0) {
        gpio_config_t bl_conf = {};
        bl_conf.pin_bit_mask = 1ULL << pins_.lcd_bl;
        bl_conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&bl_conf);
        gpio_set_level(static_cast<gpio_num_t>(pins_.lcd_bl), 1);
    }

    ESP_LOGI(TAG, "ILI9341 LCD initialized, landscape %dx%d", cfg_.screen_w, cfg_.screen_h);
    return ESP_OK;
}

// ==================== Touch ====================

esp_err_t SpitftTouchDisplay::InitTouch()
{
#if CONFIG_NODE_TOUCH_XPT2046
    esp_err_t err = xpt2046_init(pins_.touch_spi_host, pins_.touch_cs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "XPT2046 init failed: %s (touch disabled)", esp_err_to_name(err));
        return ESP_OK;  // 触摸失败不影响显示
    }

    // 竖屏 MADCTL=BGR|MX(0x48) 下逻辑 X 与玻璃物理 X 反向，起步按
    // invert_x=true；最终 swap/invert/范围以串口 raw 值校准。
    xpt2046_cal_t cal = {
        .swap_xy   = (cfg_.rotation == ScreenRotation::Landscape),
        .invert_x  = (cfg_.rotation == ScreenRotation::Portrait),
        .invert_y  = false,
        .x_min     = 300,
        .x_max     = 3800,
        .y_min     = 200,
        .y_max     = 3900,
    };
    xpt2046_set_calibration(&cal);
    ESP_LOGI(TAG, "XPT2046 touch enabled");
#endif
    return ESP_OK;
}

// ==================== LVGL ====================

esp_err_t SpitftTouchDisplay::InitLvgl()
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = lcd_io_;
    disp_cfg.panel_handle = lcd_panel_;
    disp_cfg.buffer_size = cfg_.screen_w * cfg_.buffer_rows;
    disp_cfg.double_buffer = cfg_.double_buffer;
    disp_cfg.hres = cfg_.screen_w;
    disp_cfg.vres = cfg_.screen_h;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.swap_bytes = true;
    // 关键：rotation 必须与 InitLcd() 中手动设置的 swap_xy/mirror 一致。
    // lvgl_port_add_disp() 在 ROTATION_0 下会无条件用这里的值重新下发一次
    // MADCTL（esp_lvgl_port_disp.c）；留零会把方向位清掉、与逻辑分辨率错位。
    if (cfg_.rotation == ScreenRotation::Portrait) {
        disp_cfg.rotation.swap_xy  = false;
        disp_cfg.rotation.mirror_x = true;
        disp_cfg.rotation.mirror_y = false;
    } else {
        disp_cfg.rotation.swap_xy  = true;
        disp_cfg.rotation.mirror_x = false;
        disp_cfg.rotation.mirror_y = false;
    }

    lv_disp_ = lvgl_port_add_disp(&disp_cfg);
    if (!lv_disp_) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

#if CONFIG_NODE_TOUCH_XPT2046
    // LVGL 9 输入设备注册：lv_indev_create + set_type + set_read_cb
    // 必须在 LVGL 锁内操作
    lvgl_port_lock(0);
    lv_indev_t* indev = lv_indev_create();
    if (indev) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, &SpitftTouchDisplay::IndevRead);
        lv_indev_set_user_data(indev, this);
        ESP_LOGI(TAG, "LVGL touch indev registered");
    } else {
        ESP_LOGW(TAG, "lv_indev_create failed");
    }
    lvgl_port_unlock();
#endif

    return ESP_OK;
}

void SpitftTouchDisplay::IndevRead(lv_indev_t* indev, lv_indev_data_t* data)
{
    auto* self = static_cast<SpitftTouchDisplay*>(lv_indev_get_user_data(indev));
    if (!self) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

#if CONFIG_NODE_TOUCH_XPT2046
    int16_t sx = 0, sy = 0;
    bool pressed = false;
    esp_err_t err = xpt2046_read_screen_xy(self->cfg_.screen_w, self->cfg_.screen_h,
                                           &sx, &sy, &pressed);
    if (err == ESP_OK && pressed) {
        // 按下：刷新最后有效点
        self->last_touch_x_ = sx;
        self->last_touch_y_ = sy;
        self->touch_has_point_ = true;
        data->point.x = sx;
        data->point.y = sy;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        // 抬起/出错：继续上报最后有效点（保持 press/release 在同一位置），
        // 保证 LVGL 的 CLICK 距离判定通过
        data->point.x = self->touch_has_point_ ? self->last_touch_x_ : 0;
        data->point.y = self->touch_has_point_ ? self->last_touch_y_ : 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
#else
    data->state = LV_INDEV_STATE_RELEASED;
#endif
}

// ==================== UI 构建 ====================

namespace {

// 深色主题配色
constexpr uint32_t kColorBg     = 0x101820;
constexpr uint32_t kColorCardBg = 0x1A2530;
constexpr uint32_t kColorCardBd = 0x2A3A4A;
constexpr uint32_t kColorCyan   = 0x2FD4F5;

lv_obj_t* MakeCard(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCardBg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kColorCardBd), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

// 导航按钮点击回调：user_data 携带目标屏幕
void NavBtnHandler(lv_event_t* e)
{
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (target) lv_screen_load(target);
}

// 竖屏 240x320 底部导航：3 个 72x36 按钮
void BuildNavBar(lv_obj_t* parent,
                 lv_obj_t* scr_dash, lv_obj_t* scr_auto, lv_obj_t* scr_set)
{
    constexpr int btn_w = 72, btn_h = 36, gap = 4, btn_y = 278;
    struct NavItem { const char* label; lv_obj_t* target; };
    NavItem items[3] = {
        {"仪表盘", scr_dash},
        {"自动化", scr_auto},
        {"设置",   scr_set},
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* btn = lv_btn_create(parent);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, 8 + i * (btn_w + gap), btn_y);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, items[i].label);
        lv_obj_set_style_text_font(lbl, Font16(), 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, NavBtnHandler, LV_EVENT_CLICKED, items[i].target);
    }
}

// 在卡片内创建“名称（左上）+ 数值（下方大字）”组合
void BuildDataCard(lv_obj_t* parent, int x, int y, int w, int h,
                   const char* name, lv_obj_t** value_label_out)
{
    lv_obj_t* card = MakeCard(parent, x, y, w, h);

    lv_obj_t* name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(kColorCyan), 0);
    lv_obj_set_style_text_font(name_lbl, Font16(), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t* val_lbl = lv_label_create(card);
    lv_label_set_text(val_lbl, "--.-");
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(val_lbl, Font24(), 0);
    lv_obj_align(val_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    *value_label_out = val_lbl;
}

} // namespace

void SpitftTouchDisplay::BuildAllPages()
{
    // ---------- 页面 1: 仪表盘（竖屏 240x320） ----------
    scr_dashboard_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_dashboard_, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr_dashboard_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr_dashboard_, false);

    lv_obj_t* title = lv_label_create(scr_dashboard_);
    lv_label_set_text(title, "环境监测节点");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorCyan), 0);
    lv_obj_set_style_text_font(title, Font24(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    BuildDataCard(scr_dashboard_, 8,  46, 224, 100, "温度", &lbl_temp_value_);
    BuildDataCard(scr_dashboard_, 8, 154, 224, 100, "湿度", &lbl_humi_value_);

    // ---------- 页面 2: 自动化规则 ----------
    scr_automation_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_automation_, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr_automation_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr_automation_, false);

    lv_obj_t* title2 = lv_label_create(scr_automation_);
    lv_label_set_text(title2, "自动化规则");
    lv_obj_set_style_text_color(title2, lv_color_hex(kColorCyan), 0);
    lv_obj_set_style_text_font(title2, Font24(), 0);
    lv_obj_align(title2, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* hint2 = lv_label_create(scr_automation_);
    lv_label_set_text(hint2, "暂无规则");
    lv_obj_set_style_text_color(hint2, lv_color_hex(0x90A0B0), 0);
    lv_obj_set_style_text_font(hint2, Font16(), 0);
    lv_obj_center(hint2);

    // ---------- 页面 3: 设置 ----------
    scr_settings_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_settings_, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr_settings_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr_settings_, false);

    lv_obj_t* title3 = lv_label_create(scr_settings_);
    lv_label_set_text(title3, "设置");
    lv_obj_set_style_text_color(title3, lv_color_hex(kColorCyan), 0);
    lv_obj_set_style_text_font(title3, Font24(), 0);
    lv_obj_align(title3, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* hint3 = lv_label_create(scr_settings_);
    lv_label_set_text(hint3, "手动控制待启用");
    lv_obj_set_style_text_color(hint3, lv_color_hex(0x90A0B0), 0);
    lv_obj_set_style_text_font(hint3, Font16(), 0);
    lv_obj_center(hint3);

    // ---------- 每个页面底部都放导航栏 ----------
    BuildNavBar(scr_dashboard_,  scr_dashboard_, scr_automation_, scr_settings_);
    BuildNavBar(scr_automation_, scr_dashboard_, scr_automation_, scr_settings_);
    BuildNavBar(scr_settings_,   scr_dashboard_, scr_automation_, scr_settings_);

    ESP_LOGI(TAG, "all pages built (portrait zh-CN)");
}

// ==================== 周期刷新任务 ====================

// 从 {"temp":25.30,"humidity":60.20} 这类 JSON 中取一个字段值
static bool ExtractJsonFloat(const char* json, const char* key, float* out)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    char* end = nullptr;
    float v = strtof(p + strlen(pattern), &end);
    if (end == p + strlen(pattern)) return false;
    *out = v;
    return true;
}

void SpitftTouchDisplay::RefreshTask(void* arg)
{
    auto* self = static_cast<SpitftTouchDisplay*>(arg);
    char buf[32];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        SensorReading readings[4];
        int n = self->ctx_.registry ? self->ctx_.registry->ReadAll(readings, 4) : 0;

        float temp = 0, humi = 0;
        bool have_temp = false, have_humi = false;
        for (int i = 0; i < n; ++i) {
            if (!have_temp && ExtractJsonFloat(readings[i].values_json, "temp", &temp)) {
                have_temp = true;
            }
            if (!have_humi && ExtractJsonFloat(readings[i].values_json, "humidity", &humi)) {
                have_humi = true;
            }
        }

        if (lvgl_port_lock(200)) {
            if (self->lbl_temp_value_) {
                if (have_temp) {
                    snprintf(buf, sizeof(buf), "%.1f \xC2\xB0""C", temp); // °C (UTF-8)
                    lv_label_set_text(self->lbl_temp_value_, buf);
                } else {
                    lv_label_set_text(self->lbl_temp_value_, "--.-");
                }
            }
            if (self->lbl_humi_value_) {
                if (have_humi) {
                    snprintf(buf, sizeof(buf), "%.1f %%", humi);
                    lv_label_set_text(self->lbl_humi_value_, buf);
                } else {
                    lv_label_set_text(self->lbl_humi_value_, "--.-");
                }
            }
            lvgl_port_unlock();
        }
    }
}

} // namespace esp32node
