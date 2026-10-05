// 板级装配：ESP32-S3 N16R8 + ILI9341 240x320 SPI TFT + XPT2046 触摸
//           + AHT30 温湿度模块 + PWM 风扇
//
// 构造函数内完成全部硬件初始化（对标 esp32c3_sht3x 板）：
//   SPI2 LCD 总线 / SPI3 触摸总线 -> I2C0 与 AHT30 -> LEDC 风扇
//   -> ILI9341 面板 -> XPT2046 -> Ili9341TftDisplay -> 环境采集任务
#include "wifi_board.h"
#include "config.h"
#include "ili9341_tft_display.h"

#include "aht30.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_touch_xpt2046.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

#define TAG "BoardEsp32S3TftFan"

// LCD 单帧最大传输（DMA 字节数）
static constexpr int kLcdMaxTransferBytes = DISPLAY_WIDTH * 2 * 20;

class BoardEsp32S3TftFan : public WifiBoard {
private:
    // ---- 总线 / 面板句柄 ----
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;

    Display* display_ = nullptr;
    Ili9341TftDisplay* tft_display_ = nullptr;

    // ---- AHT30 温湿度（独立组件）----
    Aht30* aht30_ = nullptr;
    bool aht30_ok_ = false;

    // ---------------- SPI 总线 ----------------
    void InitializeSpiBuses() {
        spi_bus_config_t lcd_bus_config = {};
        lcd_bus_config.mosi_io_num = LCD_PIN_MOSI;
        lcd_bus_config.miso_io_num = LCD_PIN_MISO;
        lcd_bus_config.sclk_io_num = LCD_PIN_SCLK;
        lcd_bus_config.quadwp_io_num = -1;
        lcd_bus_config.quadhd_io_num = -1;
        lcd_bus_config.max_transfer_sz = kLcdMaxTransferBytes;
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &lcd_bus_config, SPI_DMA_CH_AUTO));

        // XPT2046 单次事务仅 3 字节，独立总线轮询即可，不开 DMA
        spi_bus_config_t touch_bus_config = {};
        touch_bus_config.mosi_io_num = TOUCH_PIN_MOSI;
        touch_bus_config.miso_io_num = TOUCH_PIN_MISO;
        touch_bus_config.sclk_io_num = TOUCH_PIN_SCK;
        touch_bus_config.quadwp_io_num = -1;
        touch_bus_config.quadhd_io_num = -1;
        touch_bus_config.max_transfer_sz = 32;
        ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_SPI_HOST, &touch_bus_config, SPI_DMA_DISABLED));

        ESP_LOGI(TAG, "SPI buses ready: LCD@MOSI%d/SCLK%d, touch@MOSI%d/SCLK%d",
                 LCD_PIN_MOSI, LCD_PIN_SCLK, TOUCH_PIN_MOSI, TOUCH_PIN_SCK);
    }

    // ---------------- I2C 总线 + AHT30 ----------------
    void InitializeI2cAndSensor(i2c_master_bus_handle_t* bus) {
        i2c_master_bus_config_t i2c_config = {};
        i2c_config.i2c_port = I2C_NUM_0;
        i2c_config.sda_io_num = I2C_PIN_SDA;
        i2c_config.scl_io_num = I2C_PIN_SCL;
        i2c_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_config.glitch_ignore_cnt = 7;
        i2c_config.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_config, bus));

        aht30_ = new Aht30(*bus);
        aht30_ok_ = (aht30_->Init() == ESP_OK);
        if (!aht30_ok_) {
            ESP_LOGW(TAG, "AHT30 not detected, check wiring (addr 0x38)");
        }
    }

    // ---------------- PWM 风扇（LEDC 8bit / 25kHz）----------------
    void InitializeFan() {
        ledc_timer_config_t timer_config = {};
        timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
        timer_config.duty_resolution = LEDC_TIMER_8_BIT;
        timer_config.timer_num = LEDC_TIMER_0;
        timer_config.freq_hz = FAN_PWM_FREQ_HZ;
        timer_config.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

        ledc_channel_config_t channel_config = {};
        channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
        channel_config.channel = LEDC_CHANNEL_0;
        channel_config.timer_sel = LEDC_TIMER_0;
        channel_config.intr_type = LEDC_INTR_DISABLE;
        channel_config.gpio_num = FAN_PIN_PWM;
        channel_config.duty = 0;
        channel_config.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }

    static void FanSetPower(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        uint32_t duty = static_cast<uint32_t>(percent) * 255 / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }

    // ---------------- ILI9341 面板 ----------------
    void InitializeLcdPanel() {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_PIN_CS;
        io_config.dc_gpio_num = LCD_PIN_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = LCD_PCLK_HZ;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.trans_queue_depth = 10;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            static_cast<esp_lcd_spi_bus_handle_t>(LCD_SPI_HOST), &io_config, &lcd_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = LCD_PIN_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(lcd_io_, &panel_config, &lcd_panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel_));
        esp_lcd_panel_set_gap(lcd_panel_, 0, 0);
        esp_lcd_panel_swap_xy(lcd_panel_, false);
        esp_lcd_panel_mirror(lcd_panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel_, true));

        // 背光常亮
        gpio_config_t backlight_config = {};
        backlight_config.pin_bit_mask = 1ULL << LCD_PIN_BL;
        backlight_config.mode = GPIO_MODE_OUTPUT;
        gpio_config(&backlight_config);
        gpio_set_level(LCD_PIN_BL, 1);

        ESP_LOGI(TAG, "ILI9341 panel ready %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    // ---------------- XPT2046 触摸 ----------------
    void InitializeTouch() {
        esp_lcd_panel_io_spi_config_t touch_io_config =
            ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(TOUCH_PIN_CS);
        touch_io_config.dc_gpio_num = -1;

        esp_lcd_panel_io_handle_t touch_io = nullptr;
        esp_err_t err = esp_lcd_new_panel_io_spi(
            static_cast<esp_lcd_spi_bus_handle_t>(TOUCH_SPI_HOST),
            &touch_io_config, &touch_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "XPT2046 panel IO init failed: %s", esp_err_to_name(err));
            return;
        }

        esp_lcd_touch_config_t touch_config = {};
        touch_config.x_max = DISPLAY_HEIGHT;  // 竖屏触摸原始轴与显示轴互换
        touch_config.y_max = DISPLAY_WIDTH;
        touch_config.rst_gpio_num = GPIO_NUM_NC;
        touch_config.int_gpio_num = GPIO_NUM_NC;
        touch_config.flags.swap_xy = true;
        touch_config.flags.mirror_x = false;
        touch_config.flags.mirror_y = false;

        err = esp_lcd_touch_new_spi_xpt2046(touch_io, &touch_config, &touch_handle_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "XPT2046 init failed: %s", esp_err_to_name(err));
            touch_handle_ = nullptr;
        }
    }

    // ---------------- 显示 ----------------
    void InitializeDisplay() {
        tft_display_ = new Ili9341TftDisplay(lcd_io_, lcd_panel_,
                                             DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                             DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y
#if CONFIG_NODE_TOUCH_XPT2046
                                             , touch_handle_
#endif
        );
        // 触摸按钮 -> LEDC 风扇（显示类只发回调，不认识风扇硬件）
        tft_display_->SetFanToggleCallback([](bool on) {
            FanSetPower(on ? 100 : 0);
            ESP_LOGI(TAG, "fan %s", on ? "on" : "off");
        });
        // 长按风扇按钮 -> 手动重新配网（转发给 WifiBoard 状态机）
        tft_display_->SetProvisionRequestCallback([this]() { RequestProvisioning(); });
        display_ = tft_display_;
    }

    // ---------------- 环境采集任务 ----------------
    static void EnvPollTask(void* arg) {
        auto* self = static_cast<BoardEsp32S3TftFan*>(arg);
        while (true) {
            float temperature = NAN;
            float humidity = NAN;

            if (self->aht30_ok_ && self->aht30_ != nullptr) {
                float t = 0.0f;
                float h = 0.0f;
                if (self->aht30_->Read(&t, &h)) {
                    temperature = t;
                    humidity = h;
                }
            }

            if (self->tft_display_ != nullptr) {
                self->tft_display_->UpdateEnv(temperature, humidity);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

public:
    BoardEsp32S3TftFan() {
        InitializeSpiBuses();

        i2c_master_bus_handle_t i2c_bus = nullptr;
        InitializeI2cAndSensor(&i2c_bus);

        InitializeFan();
        InitializeLcdPanel();

#if CONFIG_NODE_TOUCH_XPT2046
        InitializeTouch();
#endif

        InitializeDisplay();

        BaseType_t ret = xTaskCreate(EnvPollTask, "env_poll", 3072, this, 2, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create env poll task");
        }

        // 联网 + 配网（WifiBoard 状态机；显示屏已就绪，须在构造末尾调用）
        InitializeNetwork();

        ESP_LOGI(TAG, "assembled: aht30=%d touch=%d",
                 aht30_ok_, touch_handle_ != nullptr);
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(BoardEsp32S3TftFan);
