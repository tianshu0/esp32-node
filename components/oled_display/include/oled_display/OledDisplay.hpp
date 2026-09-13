// oled_display 组件：SSD1315 0.96" 128x64 单色 OLED（LVGL 9 + esp_lvgl_port）
//
// 布局（128x64，英文/数字，面板已硬件旋转 180°，排针侧为上）：
//   y=1..11    页眉 10px：左 node-id，右连接状态 ADV / CONN / PAIRED
//   y=13       1px 分隔线
//   y=15/31/47 主体三行 14px（行距 16）：左标签 T/H/P 12px，右数值 14px 右对齐，读失败显示 "--"
//
// 数据来源：每 1s 通过 sensor_registry 主动采集一次。不订阅 data_pipeline 的
// 采集事件，因为 pipeline 只在配对后采样，而未配对时屏上仍需显示实时值。
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace esp32node {

class AppConfig;
class SensorRegistry;
class BlePeripheral;
class I2cBus;

class OledDisplay {
public:
    static constexpr uint8_t kAddrPrimary = 0x3C;
    static constexpr uint8_t kAddrAlt = 0x3D;
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;
    static constexpr uint32_t kRefreshMs = 1000;
    static constexpr uint32_t kTaskStack = 4096;
    static constexpr UBaseType_t kTaskPriority = 4;

    OledDisplay() = default;
    ~OledDisplay();

    // 建面板 + 挂 LVGL + 建界面 + 启动刷新任务。
    // 屏未接（0x3C/0x3D 均无应答）返回 ESP_ERR_NOT_FOUND，调用方可选择忽略。
    esp_err_t Init(AppConfig* config, SensorRegistry* registry,
                   BlePeripheral* ble, I2cBus* bus);

private:
    // 一帧要显示的内容，ok=false 表示该行读失败（显示 "--"）
    struct Reading {
        bool temp_ok = false;
        float temp_c = 0.0f;
        bool hum_ok = false;
        float humidity = 0.0f;
        bool press_ok = false;
        float pressure_hpa = 0.0f;
    };

    esp_err_t SetupPanel();
    void BuildUi();
    Reading ReadSensors() const;
    const char* StatusText() const;

    static void TaskThunk(void* ctx);
    void Run();

    AppConfig* config_ = nullptr;
    SensorRegistry* registry_ = nullptr;
    BlePeripheral* ble_ = nullptr;
    I2cBus* bus_ = nullptr;

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* disp_ = nullptr;
    uint8_t addr_ = kAddrPrimary;

    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* hum_label_ = nullptr;
    lv_obj_t* press_label_ = nullptr;

    // 上一次写入的文本：内容未变时不重设，避免无谓的整屏重绘（8KB I2C 传输）
    char last_status_[12] = {};
    char last_temp_[16] = {};
    char last_hum_[16] = {};
    char last_press_[16] = {};

    TaskHandle_t task_ = nullptr;
};

} // namespace esp32node
