// SpitftTouchDisplay 实现：SPI TFT + XPT2046 触摸 + LVGL 9 单屏 UI
//
// 初始化顺序（必须遵守）：
//   1. Board.cpp 先初始化 SPI2_HOST / SPI3_HOST 总线
//   2. Configure() 传入引脚和风扇指针
//   3. Start() 内依次：InitLcd → InitTouch → InitLvgl → BuildAllPages
//   4. RefreshTask 每秒更新湿度 + 风扇状态 + arc 进度
//
// UI 参考：参考图"快笼子风扇控制"（深蓝深色系 + 弧形湿度进度 + 双风扇控件）
//   状态栏左侧 = WiFi 标志 + IP 地址（原型图要求）：
//     STA 联网后显示真实 IP，未连接显示"未连接"（每秒轮询 esp_netif）
//   图标用 LV_SYMBOL FontAwesome（Montserrat 14 自带）：WIFI/POWER/HOME/TINT
//   风扇 icon canvas 手绘（FA 免费版无 fan）
//
// 触摸轴向已校准（竖屏 swap_xy=1, mirror_x/y=0），InitTouch 不改
#include "display_service/SpitftTouchDisplay.hpp"
#include "display_service/esp_lcd_ili9341.h"
#include "display_service/display_fonts.h"
#if CONFIG_NODE_TOUCH_XPT2046
#include "esp_lcd_touch_xpt2046.h"
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
#include "esp_netif.h"
#include "sensor_registry/SensorRegistry.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <algorithm>

namespace esp32node {

static const char* TAG = "spi-tft";

// 中文字体（SimHei 子集，含 ASCII + ° + 项目 UI 汉字）
static inline const lv_font_t* Font8()  { return &lv_font_zh8;  }
static inline const lv_font_t* Font10() { return &lv_font_zh10; }
static inline const lv_font_t* Font12() { return &lv_font_zh12; }
static inline const lv_font_t* Font14() { return &lv_font_zh14; }
static inline const lv_font_t* Font16() { return &lv_font_zh16; }
static inline const lv_font_t* Font24() { return &lv_font_zh24; }

// 内置 Montserrat 图标字体（LV_SYMBOL_WIFI/POWER/REFRESH/HOME/TINT 等）
static inline const lv_font_t* IconFont() { return &lv_font_montserrat_20; }
// 大号 Montserrat：湿度数值 44px + 电源/风扇图标 40px
static inline const lv_font_t* BigFont()  { return &lv_font_montserrat_44; }
static inline const lv_font_t* BigIcon()  { return &lv_font_montserrat_40; }

// ==================== Start ====================

esp_err_t SpitftTouchDisplay::Start(const DisplayContext& ctx)
{
    ctx_ = ctx;

    ESP_RETURN_ON_ERROR(InitLcd(),   TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(InitTouch(), TAG, "Touch init failed");
    ESP_RETURN_ON_ERROR(InitLvgl(),  TAG, "LVGL init failed");

    lvgl_port_lock(0);
    BuildAllPages();
    lv_screen_load(scr_main_);
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
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = pins_.lcd_rst;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;

    err = esp_lcd_new_panel_ili9341(lcd_io_, &panel_cfg, &lcd_panel_);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_lcd_new_panel_ili9341 failed: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_reset(lcd_panel_);
    esp_lcd_panel_init(lcd_panel_);
    esp_lcd_panel_set_gap(lcd_panel_, 0, 0);

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

    ESP_LOGI(TAG, "ILI9341 LCD initialized, portrait 240x320");
    return ESP_OK;
}

// ==================== Touch ====================

esp_err_t SpitftTouchDisplay::InitTouch()
{
#if CONFIG_NODE_TOUCH_XPT2046
    esp_lcd_panel_io_spi_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(pins_.touch_cs);
    tp_io_cfg.dc_gpio_num = -1;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)pins_.touch_spi_host, &tp_io_cfg, &touch_io_),
        TAG, "touch panel IO init failed");

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = static_cast<uint16_t>(cfg_.screen_h);  // 320（玻璃竖直通道）
    tp_cfg.y_max = static_cast<uint16_t>(cfg_.screen_w);  // 240（玻璃水平通道）
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = GPIO_NUM_NC;
    tp_cfg.flags.swap_xy  = (cfg_.rotation == ScreenRotation::Portrait);
    tp_cfg.flags.mirror_x = false;
    tp_cfg.flags.mirror_y = false;

    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_spi_xpt2046(touch_io_, &tp_cfg, &touch_handle_),
        TAG, "XPT2046 touch new failed");

    ESP_LOGI(TAG, "XPT2046 touch enabled: x_max=%u y_max=%u swap_xy=%d",
             tp_cfg.x_max, tp_cfg.y_max, tp_cfg.flags.swap_xy);
#endif
    return ESP_OK;
}

// ==================== LVGL ====================

esp_err_t SpitftTouchDisplay::InitLvgl()
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err)); return err; }

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
    if (cfg_.rotation == ScreenRotation::Portrait) {
        disp_cfg.rotation.swap_xy  = false;
        disp_cfg.rotation.mirror_x = true;
    } else {
        disp_cfg.rotation.swap_xy  = true;
        disp_cfg.rotation.mirror_x = false;
    }

    lv_disp_ = lvgl_port_add_disp(&disp_cfg);
    if (!lv_disp_) { ESP_LOGE(TAG, "lvgl_port_add_disp failed"); return ESP_FAIL; }

#if CONFIG_NODE_TOUCH_XPT2046
    lvgl_port_touch_cfg_t touch_cfg = { .disp = lv_disp_, .handle = touch_handle_ };
    lv_indev_t* indev = lvgl_port_add_touch(&touch_cfg);
    if (indev) { ESP_LOGI(TAG, "LVGL touch indev registered"); }
#endif
    return ESP_OK;
}

// ==================== UI 构建 ====================

namespace {

// 参考图"快笼子风扇控制"配色（深蓝深色系 + 青绿高亮）
constexpr uint32_t kColorBg        = 0x0F172A;
constexpr uint32_t kColorStatusBar = 0x0A1120;  // 状态栏更深
constexpr uint32_t kColorCardBg     = 0x1A2530;
constexpr uint32_t kColorCardBd     = 0x2A3A4A;
constexpr uint32_t kColorAccent     = 0x2FD4F5;
constexpr uint32_t kColorGreen      = 0x10B981;
constexpr uint32_t kColorRedOff     = 0x374151;
constexpr uint32_t kColorStateCard  = 0x1F2937;
constexpr uint32_t kColorWarnTag    = 0xF59E0B;

// 状态小标签：深色圆角底 + 小色点 + 文字（参考图 "●正常"）
// 用 flex 行布局：子元素计入 LV_SIZE_CONTENT，避免标签缩成 8x8 导致文字溢出
void MakeStatusTag(lv_obj_t* parent, const char* text, uint32_t dot_color,
                   lv_obj_t** lbl_out)
{
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
    lv_obj_set_style_text_font(lbl, Font14(), 0);
    *lbl_out = lbl;
}

// 药丸标签：纯色背景 + 圆角，无底色色点（参考图底部按钮的"已开启"/"运行中"）
void MakePillTag(lv_obj_t* parent, const char* text, uint32_t bg_color,
                 uint32_t text_color, lv_obj_t** lbl_out)
{
    lv_obj_t* tag = lv_obj_create(parent);
    lv_obj_set_style_radius(tag, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tag, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(tag, 10, 0);
    lv_obj_set_style_pad_ver(tag, 2, 0);
    lv_obj_set_style_border_width(tag, 0, 0);
    lv_obj_set_size(tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tag, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* lbl = lv_label_create(tag);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_font(lbl, Font14(), 0);
    *lbl_out = lbl;
}

// 仅用 lv_canvas_set_buffer + lv_canvas_fill_bg + lv_canvas_set_px 逐像素光栅化
// 3 叶片风扇图标（外环 + 3 叶片 + 中心毂）。避免依赖 layer/draw 向量 API，
// 用 RGB565 + 背景填所在区域底色实现与背景无缝融合。
// size: 图标像素边长；bg/ring/blade/hub: 各部分颜色
lv_obj_t* MakeFanCanvas(lv_obj_t* parent, int size,
                        lv_color_t bg, lv_color_t ring,
                        lv_color_t blade, lv_color_t hub)
{
    lv_obj_t* cv = lv_canvas_create(parent);
    uint8_t* buf = (uint8_t*)malloc(size * size * 2);  // RGB565
    lv_canvas_set_buffer(cv, buf, size, size, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(cv, bg, LV_OPA_COVER);
    lv_obj_set_style_pad_all(cv, 0, 0);
    lv_obj_set_style_border_width(cv, 0, 0);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);

    const float cx = (size - 1) * 0.5f;
    const float cy = (size - 1) * 0.5f;
    const float R  = (size - 1) * 0.5f;
    const float hub_r  = R * 0.16f;
    const float b_in   = hub_r;
    const float b_out  = R - 1.2f;
    const float blade_half = 36.0f;   // 叶片半角(°)，宽 72° 间隙 48°

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy);
            if (r > R) continue;                          // 圆盘外保持底色
            // 0° 在正上，顺时针递增
            float ang = atan2f(dx, -dy) * 180.0f / 3.14159265f;
            if (ang < 0) ang += 360.0f;
            lv_color_t c;
            bool draw = false;
            if (r <= hub_r) { c = hub; draw = true; }                 // 中心毂
            else if (r >= R - 1.0f) { c = ring; draw = true; }       // 外环
            else if (r >= b_in && r <= b_out) {
                float d0 = fmodf(ang, 120.0f);                        // 在 120° 扇区内位置
                float dt = (d0 > 60.0f) ? 120.0f - d0 : d0;           // 到最近叶片中心角距
                if (dt <= blade_half) { c = blade; draw = true; }
            }
            if (draw) lv_canvas_set_px(cv, x, y, c, LV_OPA_COVER);
        }
    }
    return cv;
}

} // namespace

// ==================== 回调 ====================

void SpitftTouchDisplay::FanBtnHandler(lv_event_t* e)
{
    auto* self = static_cast<SpitftTouchDisplay*>(lv_event_get_user_data(e));
    if (!self || !self->fan_) return;
    if (self->fan_->IsRunning()) {
        self->fan_->SetPower(0, true);
        ESP_LOGI(TAG, "fan button: stop");
    } else {
        self->fan_->SetPower(100, true);
        ESP_LOGI(TAG, "fan button: start 100%%");
    }
    self->UpdateFanButton();
}

// 刷新左按钮 + 右状态卡颜色和文案（须持 LVGL 锁）
void SpitftTouchDisplay::UpdateFanButton()
{
    if (!btn_fan_) return;
    const bool running = fan_ && fan_->IsRunning();

    // 左按钮底色：运行=绿，停止=灰
    lv_obj_set_style_bg_color(btn_fan_,
                              lv_color_hex(running ? kColorGreen : kColorRedOff), 0);

    // 左按钮药丸标签
    if (lbl_fan_switch_st_) {
        lv_label_set_text(lbl_fan_switch_st_, running ? "已开启" : "已关闭");
        lv_obj_t* pill = lv_obj_get_parent(lbl_fan_switch_st_);
        lv_obj_set_style_bg_color(pill, lv_color_hex(running ? 0x065F46 : 0x1F2937), 0);
        lv_obj_set_style_text_color(lbl_fan_switch_st_,
                                    lv_color_hex(running ? 0xFFFFFF : 0x90A0B0), 0);
    }

    // 右状态卡药丸标签 / V2 数据面板状态文字
    if (lbl_fan_state_st_) {
        lv_label_set_text(lbl_fan_state_st_, running ? "运行中" : "已停止");
#if FAN_UI_LAYOUT == 1
        lv_obj_t* pill = lv_obj_get_parent(lbl_fan_state_st_);
        lv_obj_set_style_bg_color(pill, lv_color_hex(running ? 0x1E3A5F : 0x1F2937), 0);
        lv_obj_set_style_text_color(lbl_fan_state_st_,
                                    lv_color_hex(running ? 0x60A5FA : 0x90A0B0), 0);
#else
        lv_obj_set_style_text_color(lbl_fan_state_st_,
                                    lv_color_hex(running ? 0x00FF88 : 0x90A0B0), 0);
#endif
    }
}

// ==================== BuildAllPages ====================
// UI 布局选择：1 = 双卡布局（左风扇开关 + 右风扇状态）  2 = 大按钮布局（合并面板 + 底部大绿按钮）
#define FAN_UI_LAYOUT 2

void SpitftTouchDisplay::BuildAllPages()
{
    scr_main_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_main_, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr_main_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr_main_, false);
    lv_obj_set_style_pad_all(scr_main_, 0, 0);

    // ============ 布局分支 ============
#if FAN_UI_LAYOUT == 1
    // ============ V1 顶部状态栏（Y=0, H=42） ============
    lv_obj_t* sb = lv_obj_create(scr_main_);
    lv_obj_set_size(sb, 240, 42);
    lv_obj_set_style_radius(sb, 0, 0);
    lv_obj_set_style_bg_color(sb, lv_color_hex(kColorStatusBar), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* wifi_icon = lv_label_create(sb);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_text_font(wifi_icon, IconFont(), 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, 10, 8);

    lbl_conn_ = lv_label_create(sb);
    lv_label_set_text(lbl_conn_, "未连接");
    lv_obj_set_style_text_color(lbl_conn_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_conn_, Font16(), 0);
    lv_obj_align(lbl_conn_, LV_ALIGN_TOP_LEFT, 35, 3);

    lbl_wifi_st_ = lv_label_create(sb);
    lv_label_set_text(lbl_wifi_st_, "");
    lv_obj_set_style_text_color(lbl_wifi_st_, lv_color_hex(0x90A0B0), 0);
    lv_obj_set_style_text_font(lbl_wifi_st_, Font10(), 0);
    lv_obj_align(lbl_wifi_st_, LV_ALIGN_TOP_LEFT, 35, 24);

    lbl_time_ = lv_label_create(sb);
    lv_label_set_text(lbl_time_, "--:--");
    lv_obj_set_style_text_color(lbl_time_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_time_, IconFont(), 0);
    lv_obj_align(lbl_time_, LV_ALIGN_TOP_RIGHT, -10, 2);

    lbl_date_ = lv_label_create(sb);
    lv_label_set_text(lbl_date_, "");
    lv_obj_set_style_text_color(lbl_date_, lv_color_hex(0x90A0B0), 0);
    lv_obj_set_style_text_font(lbl_date_, Font14(), 0);
    lv_obj_align(lbl_date_, LV_ALIGN_TOP_RIGHT, -10, 24);

    // V1 分割线 Y=41
    lv_obj_t* hline = lv_obj_create(scr_main_);
    lv_obj_set_size(hline, 224, 1);
    lv_obj_set_pos(hline, 8, 41);
    lv_obj_set_style_bg_color(hline, lv_color_hex(0x144B8C), 0);
    lv_obj_set_style_bg_opa(hline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hline, 0, 0);
    lv_obj_set_style_border_width(hline, 0, 0);
    lv_obj_clear_flag(hline, LV_OBJ_FLAG_SCROLLABLE);

    // V1 标题
    lv_obj_t* title = lv_label_create(scr_main_);
    lv_label_set_text(title, "快笼子风扇");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, Font24(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    // V1 湿度卡
    lv_obj_t* humi_card = lv_obj_create(scr_main_);
    lv_obj_set_size(humi_card, 220, 90);
    lv_obj_set_pos(humi_card, 10, 84);
    lv_obj_set_style_radius(humi_card, 12, 0);
    lv_obj_set_style_bg_color(humi_card, lv_color_hex(kColorCardBg), 0);
    lv_obj_set_style_bg_opa(humi_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(humi_card, lv_color_hex(kColorCardBd), 0);
    lv_obj_set_style_border_width(humi_card, 1, 0);
    lv_obj_set_style_pad_all(humi_card, 0, 0);
    lv_obj_clear_flag(humi_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* water_icon = lv_label_create(humi_card);
    lv_label_set_text(water_icon, LV_SYMBOL_TINT);
    lv_obj_set_style_text_color(water_icon, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_text_font(water_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(water_icon, LV_ALIGN_TOP_LEFT, 13, 10);

    lv_obj_t* humi_name = lv_label_create(humi_card);
    lv_label_set_text(humi_name, "湿度");
    lv_obj_set_style_text_color(humi_name, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_text_font(humi_name, Font16(), 0);
    lv_obj_align(humi_name, LV_ALIGN_TOP_LEFT, 45, 12);

    lbl_humi_value_ = lv_label_create(humi_card);
    lv_label_set_text(lbl_humi_value_, "--");
    lv_obj_set_style_text_color(lbl_humi_value_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_humi_value_, BigFont(), 0);
    lv_obj_align(lbl_humi_value_, LV_ALIGN_TOP_LEFT, 18, 30);

    lv_obj_t* humi_pct = lv_label_create(humi_card);
    lv_label_set_text(humi_pct, "%");
    lv_obj_set_style_text_color(humi_pct, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(humi_pct, Font24(), 0);
    lv_obj_align_to(humi_pct, lbl_humi_value_, LV_ALIGN_OUT_RIGHT_MID, 4, 6);

    MakeStatusTag(humi_card, "正常", kColorGreen, &lbl_humi_status_);
    lv_obj_align(lv_obj_get_parent(lbl_humi_status_), LV_ALIGN_BOTTOM_LEFT, 14, -4);

    arc_humi_ = lv_arc_create(humi_card);
    lv_obj_set_size(arc_humi_, 76, 76);
    lv_obj_align(arc_humi_, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_arc_set_rotation(arc_humi_, 135);
    lv_arc_set_bg_angles(arc_humi_, 0, 270);
    lv_arc_set_mode(arc_humi_, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(arc_humi_, 0, 100);
    lv_arc_set_value(arc_humi_, 0);
    lv_obj_set_style_opa(arc_humi_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc_humi_, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_humi_, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_humi_, lv_color_hex(kColorCardBd), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_humi_, 8, LV_PART_MAIN);
    lv_obj_clear_flag(arc_humi_, LV_OBJ_FLAG_CLICKABLE);

    lbl_arc_pct_ = lv_label_create(arc_humi_);
    lv_label_set_text(lbl_arc_pct_, "--%");
    lv_obj_set_style_text_color(lbl_arc_pct_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_arc_pct_, Font14(), 0);
    lv_obj_center(lbl_arc_pct_);

    // V1 底部双卡
    btn_fan_ = lv_btn_create(scr_main_);
    lv_obj_set_size(btn_fan_, 106, 132);
    lv_obj_set_pos(btn_fan_, 10, 180);
    lv_obj_set_style_radius(btn_fan_, 12, 0);
    lv_obj_set_style_bg_color(btn_fan_, lv_color_hex(kColorGreen), 0);
    lv_obj_set_style_bg_opa(btn_fan_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_fan_, 0, 0);
    lv_obj_set_style_pad_all(btn_fan_, 0, 0);
    lv_obj_set_style_shadow_width(btn_fan_, 0, 0);
    lv_obj_clear_flag(btn_fan_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn_fan_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(btn_fan_, &SpitftTouchDisplay::FanBtnHandler, LV_EVENT_CLICKED, this);

    lv_obj_t* power_icon = lv_label_create(btn_fan_);
    lv_label_set_text(power_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(power_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(power_icon, BigIcon(), 0);
    lv_obj_align(power_icon, LV_ALIGN_TOP_MID, 0, 12);

    lbl_fan_switch_ = lv_label_create(btn_fan_);
    lv_label_set_text(lbl_fan_switch_, "风扇开关");
    lv_obj_set_style_text_color(lbl_fan_switch_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_fan_switch_, Font16(), 0);
    lv_obj_align(lbl_fan_switch_, LV_ALIGN_TOP_MID, 0, 62);

    MakePillTag(btn_fan_, "已关闭", 0x065F46, 0xFFFFFF, &lbl_fan_switch_st_);
    lv_obj_align(lv_obj_get_parent(lbl_fan_switch_st_), LV_ALIGN_BOTTOM_MID, 0, -10);

    fan_state_card_ = lv_obj_create(scr_main_);
    lv_obj_set_size(fan_state_card_, 106, 132);
    lv_obj_set_pos(fan_state_card_, 124, 180);
    lv_obj_set_style_radius(fan_state_card_, 12, 0);
    lv_obj_set_style_bg_color(fan_state_card_, lv_color_hex(kColorStateCard), 0);
    lv_obj_set_style_bg_opa(fan_state_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fan_state_card_, 0, 0);
    lv_obj_set_style_pad_all(fan_state_card_, 0, 0);
    lv_obj_clear_flag(fan_state_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(fan_state_card_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* fan_icon = lv_label_create(fan_state_card_);
    lv_label_set_text(fan_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(fan_icon, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_text_font(fan_icon, BigIcon(), 0);
    lv_obj_align(fan_icon, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t* st_title = lv_label_create(fan_state_card_);
    lv_label_set_text(st_title, "风扇状态");
    lv_obj_set_style_text_color(st_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(st_title, Font16(), 0);
    lv_obj_align(st_title, LV_ALIGN_TOP_MID, 0, 62);

    MakePillTag(fan_state_card_, "已停止", 0x1E3A5F, 0x60A5FA, &lbl_fan_state_st_);
    lv_obj_align(lv_obj_get_parent(lbl_fan_state_st_), LV_ALIGN_BOTTOM_MID, 0, -10);

#else  // ===== FAN_UI_LAYOUT == 2 =====

    // ============ V2 顶部状态栏（Y=0, H=48, #09254C） ============
    // 加高到 48px 以容纳右上 时间 + 日期(年月日 周X) 两行
    lv_obj_t* sb = lv_obj_create(scr_main_);
    lv_obj_set_size(sb, 240, 48);
    lv_obj_set_style_radius(sb, 0, 0);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x09254C), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi 图标 (10, 10) 青色
    lv_obj_t* wifi_icon = lv_label_create(sb);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x12D8EF), 0);
    lv_obj_set_style_text_font(wifi_icon, IconFont(), 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, 10, 10);

    // 状态指示圆点 (39, 13) 绿色
    dot_conn_ = lv_obj_create(sb);
    lv_obj_set_size(dot_conn_, 7, 7);
    lv_obj_set_style_radius(dot_conn_, 4, 0);
    lv_obj_set_style_bg_color(dot_conn_, lv_color_hex(0x19D9A0), 0);
    lv_obj_set_style_bg_opa(dot_conn_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot_conn_, 0, 0);
    lv_obj_set_pos(dot_conn_, 39, 13);
    lv_obj_clear_flag(dot_conn_, LV_OBJ_FLAG_SCROLLABLE);

    // "已连接" (49, 8) Font14
    lbl_conn_ = lv_label_create(sb);
    lv_label_set_text(lbl_conn_, "未连接");
    lv_obj_set_style_text_color(lbl_conn_, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_text_font(lbl_conn_, Font14(), 0);
    lv_obj_align(lbl_conn_, LV_ALIGN_TOP_LEFT, 49, 8);

    // IP 辅助文字 (49, 24) Font10
    lbl_wifi_st_ = lv_label_create(sb);
    lv_label_set_text(lbl_wifi_st_, "");
    lv_obj_set_style_text_color(lbl_wifi_st_, lv_color_hex(0x8DB9E8), 0);
    lv_obj_set_style_text_font(lbl_wifi_st_, Font10(), 0);
    lv_obj_align(lbl_wifi_st_, LV_ALIGN_TOP_LEFT, 50, 32);

    // 时间 (右上, Montserrat 20)
    lbl_time_ = lv_label_create(sb);
    lv_label_set_text(lbl_time_, "--:--");
    lv_obj_set_style_text_color(lbl_time_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_time_, IconFont(), 0);
    lv_obj_align(lbl_time_, LV_ALIGN_TOP_RIGHT, -10, 4);

    // 日期 "YYYY-MM-DD 周X" (右上, 时间下方) Font14
    lbl_date_ = lv_label_create(sb);
    lv_label_set_text(lbl_date_, "");
    lv_obj_set_style_text_color(lbl_date_, lv_color_hex(0x8DB9E8), 0);
    lv_obj_set_style_text_font(lbl_date_, Font10(), 0);
    lv_obj_align(lbl_date_, LV_ALIGN_TOP_RIGHT, -10, 32);

    // V2 分割线 Y=47
    lv_obj_t* hline = lv_obj_create(scr_main_);
    lv_obj_set_size(hline, 224, 1);
    lv_obj_set_pos(hline, 8, 47);
    lv_obj_set_style_bg_color(hline, lv_color_hex(0x1768B7), 0);
    lv_obj_set_style_bg_opa(hline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hline, 0, 0);
    lv_obj_set_style_border_width(hline, 0, 0);
    lv_obj_clear_flag(hline, LV_OBJ_FLAG_SCROLLABLE);

    // ============ V2 标题区（Y=48~76, H=28） ============
    // 居中的 [风扇图标][快笼子风扇控制] 行容器
    lv_obj_t* title_row = lv_obj_create(scr_main_);
    lv_obj_set_size(title_row, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 6, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 0, 50);

    // 标题左侧风扇图标（单色青，简笔风 = SVG #3）
    MakeFanCanvas(title_row, 18,
        lv_color_hex(kColorBg), lv_color_hex(0x12D8EF),
        lv_color_hex(0x12D8EF), lv_color_hex(0x12D8EF));
    lv_obj_t* title = lv_label_create(title_row);
    lv_label_set_text(title, "快笼子风扇控制");
    lv_obj_set_style_text_color(title, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_text_font(title, Font16(), 0);

    // V2 第二分割线 Y=75
    lv_obj_t* hline2 = lv_obj_create(scr_main_);
    lv_obj_set_size(hline2, 224, 1);
    lv_obj_set_pos(hline2, 8, 75);
    lv_obj_set_style_bg_color(hline2, lv_color_hex(0x143A65), 0);
    lv_obj_set_style_bg_opa(hline2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hline2, 0, 0);
    lv_obj_set_style_border_width(hline2, 0, 0);
    lv_obj_clear_flag(hline2, LV_OBJ_FLAG_SCROLLABLE);

    // ============ V2 信息卡片（X=8, Y=81, W=224, H=91） ============
    lv_obj_t* data_card = lv_obj_create(scr_main_);
    lv_obj_set_size(data_card, 224, 91);
    lv_obj_set_pos(data_card, 8, 81);
    lv_obj_set_style_radius(data_card, 10, 0);
    lv_obj_set_style_bg_color(data_card, lv_color_hex(0x0B2C59), 0);
    lv_obj_set_style_bg_opa(data_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(data_card, lv_color_hex(0x1768C1), 0);
    lv_obj_set_style_border_width(data_card, 1, 0);
    lv_obj_set_style_pad_all(data_card, 0, 0);
    lv_obj_clear_flag(data_card, LV_OBJ_FLAG_SCROLLABLE);

    // --- 左半：湿度 ---
    // 水滴图标 (card-rel 11,9)
    lv_obj_t* water_icon = lv_label_create(data_card);
    lv_label_set_text(water_icon, LV_SYMBOL_TINT);
    lv_obj_set_style_text_color(water_icon, lv_color_hex(0x12D8EF), 0);
    lv_obj_set_style_text_font(water_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(water_icon, LV_ALIGN_TOP_LEFT, 11, 9);

    // "湿度" (card-rel 39,11)
    lv_obj_t* humi_name = lv_label_create(data_card);
    lv_label_set_text(humi_name, "湿度");
    lv_obj_set_style_text_color(humi_name, lv_color_hex(0xB9D8F7), 0);
    lv_obj_set_style_text_font(humi_name, Font14(), 0);
    lv_obj_align(humi_name, LV_ALIGN_TOP_LEFT, 39, 11);

    // 大数字 (card-rel 7,36) BigIcon 40px
    lbl_humi_value_ = lv_label_create(data_card);
    lv_label_set_text(lbl_humi_value_, "--");
    lv_obj_set_style_text_color(lbl_humi_value_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_humi_value_, BigIcon(), 0);
    lv_obj_align(lbl_humi_value_, LV_ALIGN_TOP_LEFT, 35, 25);

    // "%" (IconFont 20, 紧贴数字)
    // 注意：LVGL9 的 lv_obj_align_to 是一次性绝对定位，数字文本从 "--"
    // 变为 "73"/"100" 后宽度变化不会带动 "%"，故保存指针，每次刷新后重新对齐。
    lbl_humi_pct_ = lv_label_create(data_card);
    lv_label_set_text(lbl_humi_pct_, "%");
    lv_obj_set_style_text_color(lbl_humi_pct_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_humi_pct_, IconFont(), 0);
    lv_obj_align_to(lbl_humi_pct_, lbl_humi_value_, LV_ALIGN_OUT_RIGHT_MID, 2, 4);

    // 状态胶囊 (card-rel 30,67)
    MakeStatusTag(data_card, "正常", 0x20E3AA, &lbl_humi_status_);
    lv_obj_align(lv_obj_get_parent(lbl_humi_status_), LV_ALIGN_TOP_LEFT, 35, 67);

    // 垂直分割线 (card-rel 112,9, H=73)
    lv_obj_t* vline = lv_obj_create(data_card);
    lv_obj_set_size(vline, 1, 73);
    lv_obj_set_style_bg_color(vline, lv_color_hex(0x1768C1), 0);
    lv_obj_set_style_bg_opa(vline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vline, 0, 0);
    lv_obj_set_style_border_width(vline, 0, 0);
    lv_obj_align(vline, LV_ALIGN_TOP_LEFT, 112, 9);
    lv_obj_clear_flag(vline, LV_OBJ_FLAG_SCROLLABLE);

    // --- 右半：风扇状态 ---
    // [风扇图标][风扇状态] 行（图标在文字左边）
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

    // 状态左侧风扇图标（蓝环青叶白毂 = SVG #1 风扇.svg）
    MakeFanCanvas(st_row, 20,
        lv_color_hex(0x0B2C59), lv_color_hex(0x1C86E5),
        lv_color_hex(0x12D8EF), lv_color_hex(0xFFFFFF));
    lv_obj_t* st_title = lv_label_create(st_row);
    lv_label_set_text(st_title, "风扇状态");
    lv_obj_set_style_text_color(st_title, lv_color_hex(0xB9D8F7), 0);
    lv_obj_set_style_text_font(st_title, Font14(), 0);

    // "运行中"/"已停止" (card-rel 居中, Font16)
    lbl_fan_state_st_ = lv_label_create(data_card);
    lv_label_set_text(lbl_fan_state_st_, "已停止");
    lv_obj_set_style_text_color(lbl_fan_state_st_, lv_color_hex(0x90A0B0), 0);
    lv_obj_set_style_text_font(lbl_fan_state_st_, Font16(), 0);
    lv_obj_set_width(lbl_fan_state_st_, 104);
    lv_obj_set_style_text_align(lbl_fan_state_st_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_fan_state_st_, LV_ALIGN_TOP_LEFT, 114, 56);

    // ============ V2 底部大开关（X=8, Y=181, W=224, H=130） ============
    btn_fan_ = lv_btn_create(scr_main_);
    lv_obj_set_size(btn_fan_, 224, 130);
    lv_obj_set_pos(btn_fan_, 8, 181);
    lv_obj_set_style_radius(btn_fan_, 14, 0);
    lv_obj_set_style_bg_color(btn_fan_, lv_color_hex(0x18304E), 0);
    lv_obj_set_style_bg_opa(btn_fan_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_fan_, lv_color_hex(0x48617F), 0);
    lv_obj_set_style_border_width(btn_fan_, 2, 0);
    lv_obj_set_style_pad_all(btn_fan_, 0, 0);
    lv_obj_set_style_shadow_width(btn_fan_, 0, 0);
    lv_obj_clear_flag(btn_fan_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn_fan_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(btn_fan_, &SpitftTouchDisplay::FanBtnHandler, LV_EVENT_CLICKED, this);

    // 电源图标 (居中, Y=10, BigFont 44px)
    lv_obj_t* power_icon = lv_label_create(btn_fan_);
    lv_label_set_text(power_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(power_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(power_icon, BigFont(), 0);
    lv_obj_align(power_icon, LV_ALIGN_TOP_MID, 0, 10);

    // "风扇开关" (居中, Y=62, Font24)
    lbl_fan_switch_ = lv_label_create(btn_fan_);
    lv_label_set_text(lbl_fan_switch_, "风扇开关");
    lv_obj_set_style_text_color(lbl_fan_switch_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_fan_switch_, Font24(), 0);
    lv_obj_align(lbl_fan_switch_, LV_ALIGN_TOP_MID, 0, 62);

    // "点击控制风扇" (居中, Y=92, Font14)
    lv_obj_t* hint = lv_label_create(btn_fan_);
    lv_label_set_text(hint, "点击控制风扇");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB9D8F7), 0);
    lv_obj_set_style_text_font(hint, Font14(), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 92);

    // ============ V2 底部（无 MQTT 栏，大开关直达屏幕底部附近） ============
    // 用户反馈 MQTT 显示多余（且 Font10 无汉字会出方块），已移除整条底部状态栏。

    // V2 不使用 arc / pill tags（date/time 已在状态栏创建）
    arc_humi_ = nullptr;
    lbl_arc_pct_ = nullptr;
    lbl_fan_switch_st_ = nullptr;
    fan_state_card_ = nullptr;

#endif  // FAN_UI_LAYOUT

    UpdateFanButton();
    ESP_LOGI(TAG, "UI built (layout v%d)", FAN_UI_LAYOUT);
}

// ==================== RefreshTask ====================

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

// 读 STA IPv4 字符串：esp_netif 未初始化 / 未拿到 DHCP IP 时返回 false。
// （netif 列表在 wifi_portal 初始化前为空，此时直接返回 NULL，安全）
static bool GetStaIp(char* out, size_t len)
{
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(out, len, IPSTR, IP2STR(&info.ip));
    return true;
}

// 获取本地时间：time="HH:MM"，date="YYYY-MM-DD 周X"（状态栏右侧两行）。
// 系统时间未校时（NTP 未同步，年份 < 2024）时返回 false，调用方显示占位。
static bool GetLocalTime(char* time_buf, size_t time_len,
                         char* date_buf, size_t date_len)
{
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) return false;  // 未校时

    snprintf(time_buf, time_len, "%02d:%02d", t.tm_hour, t.tm_min);
    if (date_buf && date_len > 0) {
        static const char* kWdays[] = {"周日","周一","周二","周三","周四","周五","周六"};
        snprintf(date_buf, date_len, "%04d-%02d-%02d %s",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, kWdays[t.tm_wday % 7]);
    }
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

        float humi = 0;
        bool have_humi = false;
        for (int i = 0; i < n; ++i) {
            if (!have_humi && ExtractJsonFloat(readings[i].values_json, "humidity", &humi)) {
                have_humi = true;
            }
        }

        if (lvgl_port_lock(200)) {
            // 状态栏 WiFi：已连接显示状态+IP，未连接显示"未连接"
            char ipbuf[16];
            bool connected = GetStaIp(ipbuf, sizeof(ipbuf));
            if (self->lbl_conn_) {
                lv_label_set_text(self->lbl_conn_, connected ? "已连接" : "未连接");
            }
            if (self->lbl_wifi_st_) {
                lv_label_set_text(self->lbl_wifi_st_, connected ? ipbuf : "");
            }
            // V2 顶部状态指示圆点：连接=绿，未连接=橙
            if (self->dot_conn_) {
                lv_obj_set_style_bg_color(self->dot_conn_,
                    lv_color_hex(connected ? 0x19D9A0 : 0xF59E0B), 0);
            }
            // 状态栏时间+日期：NTP 校时后显示，未校时占位
            // （V2 状态栏含 lbl_date_；V1 含 lbl_date_；均独立判空以兼容）
            if (self->lbl_time_) {
                char tbuf[8], dbuf[24];
                if (GetLocalTime(tbuf, sizeof(tbuf), dbuf, sizeof(dbuf))) {
                    lv_label_set_text(self->lbl_time_, tbuf);
                    if (self->lbl_date_) lv_label_set_text(self->lbl_date_, dbuf);
                } else {
                    lv_label_set_text(self->lbl_time_, "--:--");
                    if (self->lbl_date_) lv_label_set_text(self->lbl_date_, "");
                }
            }
            // 湿度数值 + 弧形仪表 + 状态标签
            if (self->lbl_humi_value_) {
                if (have_humi) {
                    snprintf(buf, sizeof(buf), "%.0f", humi);
                    lv_label_set_text(self->lbl_humi_value_, buf);
                    // 数字宽度随位数变化（"73"→"100"），"%" 必须重新贴到右缘
                    if (self->lbl_humi_pct_) {
                        lv_obj_align_to(self->lbl_humi_pct_, self->lbl_humi_value_,
                                        LV_ALIGN_OUT_RIGHT_MID, 2, 4);
                    }
                    int pct = std::clamp(static_cast<int>(humi + 0.5f), 0, 100);
                    if (self->arc_humi_)    lv_arc_set_value(self->arc_humi_, pct);
                    if (self->lbl_arc_pct_) {
                        snprintf(buf, sizeof(buf), "%d%%", pct);
                        lv_label_set_text(self->lbl_arc_pct_, buf);
                    }
                    if (self->lbl_humi_status_) {
                        lv_label_set_text(self->lbl_humi_status_, pct > 70 ? "偏高" : "正常");
                    }
                } else {
                    lv_label_set_text(self->lbl_humi_value_, "--");
                    // 数字宽度变化后（如 "100"→"--"）重新对齐 "%"，避免重叠/错位
                    if (self->lbl_humi_pct_) {
                        lv_obj_align_to(self->lbl_humi_pct_, self->lbl_humi_value_,
                                        LV_ALIGN_OUT_RIGHT_MID, 2, 4);
                    }
                    if (self->arc_humi_)    lv_arc_set_value(self->arc_humi_, 0);
                    if (self->lbl_arc_pct_) lv_label_set_text(self->lbl_arc_pct_, "--%");
                    if (self->lbl_humi_status_)
                        lv_label_set_text(self->lbl_humi_status_, "--");
                }
            }
            self->UpdateFanButton();
            lvgl_port_unlock();
        }
    }
}

} // namespace esp32node
