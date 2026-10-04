#include "ble_board.h"
#include "config.h"
#include "ssd1315_oled_display.h"
#include "voc21.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <esp_lcd_panel_ops.h>
#include <esp_oled_ssd1315.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "BoardEsp32C3Voc21"

class BoardEsp32C3Voc21 : public BleBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    // 具体显示类型：传感器数值推送用（显示初始化失败走 NoDisplay 时保持 nullptr）
    Ssd1315OledDisplay* oled_display_ = nullptr;
    Voc21* voc21_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1315Display() {
        // SSD1315 config
        // 用零初始化 + 逐字段赋值，避免 C++ 指定初始化器字段顺序必须与
        // esp_lcd_panel_io_i2c_config_t 声明顺序一致的约束（IDF 各版本顺序有差异）。
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.scl_speed_hz = 400 * 1000;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1315 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1315_config_t ssd1315_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1315_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1315(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1315 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // SSD1315 单色 OLED 需要反色，否则像素亮灭与背景颠倒（与官方示例一致）
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        oled_display_ = new Ssd1315OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = oled_display_;
    }

    void InitializeVoc21() {
        // 21VOC 模块独占 UART1，9600 8N1，无流控
        uart_config_t uart_config = {};
        uart_config.baud_rate = 9600;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.source_clk = UART_SCLK_DEFAULT;

        ESP_ERROR_CHECK(uart_driver_install(VOC_UART_PORT, 1024, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(VOC_UART_PORT, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(VOC_UART_PORT, VOC_UART_TX_PIN, VOC_UART_RX_PIN,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

        // 模块上电即连续上报，Init 不做在线探测；首帧校验通过前 Read() 返回 false
        voc21_ = new Voc21(VOC_UART_PORT);
        voc21_->Init();

        BaseType_t ret = xTaskCreate(SensorPollTask, "voc21_poll", 3072, this, 2, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create 21VOC poll task");
        }
    }

    // 周期采集 21VOC 并把 TVOC/甲醛推送到显示屏
    static void SensorPollTask(void* arg) {
        auto* self = static_cast<BoardEsp32C3Voc21*>(arg);
        while (true) {
            uint16_t tvoc = 0;
            uint16_t ch2o = 0;
            // 仅取 TVOC / CH2O，eCO2/温湿度本板不使用
            if (self->voc21_->Read(&tvoc, &ch2o, nullptr, nullptr, nullptr)
                && self->oled_display_ != nullptr) {
                self->oled_display_->UpdateVoc21(tvoc, ch2o);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

public:
    BoardEsp32C3Voc21() : BleBoard() {
        InitializeDisplayI2c();
        InitializeSsd1315Display();
        InitializeVoc21();
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

};

DECLARE_BOARD(BoardEsp32C3Voc21);
