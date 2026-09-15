#include "display_service/Ssd1306Display.hpp"

#include <cstring>

#include "app_config/AppConfig.hpp"
#include "ble_peripheral/BlePeripheral.hpp"
#include "i2c_bus/I2cBus.hpp"
#include "hardware_context/HardwareContext.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "display_service/Screen.hpp"

#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"

namespace esp32node {

static const char* TAG = "display";

Ssd1306Display::~Ssd1306Display()
{
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
}

esp_err_t Ssd1306Display::Start(const DisplayContext& ctx)
{
    if (ctx.config == nullptr || ctx.registry == nullptr ||
        ctx.ble == nullptr || ctx.hw == nullptr || ctx.hw->i2c == nullptr ||
        ctx.screen == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx_ = ctx;

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
    disp_cfg.buffer_size = kWidth * kHeight;
    disp_cfg.double_buffer = false;
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
        lv_obj_t* scr = lv_display_get_screen_active(disp_);
        ctx_.screen->Build(scr, ctx.config->NodeId().c_str(), *ctx.registry);
        lvgl_port_unlock();
    }

    BaseType_t ok = xTaskCreate(TaskThunk, "display", kTaskStack, this,
                                kTaskPriority, &task_);
    if (ok != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "xTaskCreate failed, free heap=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ssd1306 ready: %dx%d addr=0x%02X refresh=%lums free heap=%u",
             kWidth, kHeight, addr_, static_cast<unsigned long>(kRefreshMs),
             static_cast<unsigned>(esp_get_free_heap_size()));
    return ESP_OK;
}

esp_err_t Ssd1306Display::SetupPanel()
{
    I2cBus* bus = ctx_.hw->i2c;

    if (bus->Probe(kAddrPrimary)) {
        addr_ = kAddrPrimary;
    } else if (bus->Probe(kAddrAlt)) {
        addr_ = kAddrAlt;
    } else {
        ESP_LOGW(TAG, "oled not found at 0x3C/0x3D, check wiring");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = addr_;
    io_cfg.scl_speed_hz = I2cBus::kDefaultClkHz;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 6;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus->Handle(), &io_cfg, &io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new panel io failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_ssd1306_config_t ssd_cfg = {};
    ssd_cfg.height = kHeight;  // 128x32：仅 32 行（4 个 page）
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = -1;
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

    // 安装方向以排针侧为上：SEG + COM 同时重映射（0xA1 + 0xC8 = 旋转 180°）
    err = esp_lcd_panel_mirror(panel_, true, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel mirror failed: %s", esp_err_to_name(err));
    }
    return err;
}

const char* Ssd1306Display::StatusText() const
{
    if (ctx_.ble->IsPaired()) {
        return "PAIRED";
    }
    if (ctx_.ble->IsConnected()) {
        return "CONN";
    }
    return "ADV";
}

void Ssd1306Display::TaskThunk(void* ctx)
{
    static_cast<Ssd1306Display*>(ctx)->Run();
}

void Ssd1306Display::Run()
{
    while (true) {
        SensorReading samples[SensorRegistry::kMaxSensors] = {};

        ctx_.hw->i2c->Lock();
        int n = ctx_.registry->ReadAll(samples, SensorRegistry::kMaxSensors);
        ctx_.hw->i2c->Unlock();

        if (lvgl_port_lock(0)) {
            ctx_.screen->SetStatus(StatusText());
            ctx_.screen->Update(samples, n, *ctx_.registry);
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(kRefreshMs));
    }
}

} // namespace esp32node
