// XPT2046 触摸驱动实现（运行在独立 SPI3_HOST 总线上，与 LCD 隔离）
//
// 工作流程：
//   1. 主机拉低 T_CS，发送 8-bit 命令（通道 + 分辨率 + 电源）
//   2. 随后 16 个时钟读回 12-bit ADC（高 4 位在第 2 字节低半）
//   3. 事务结束 CS 自动拉高
//
// XPT2046 命令：0x90=X 轴(差分)，0xD0=Y 轴(差分)，12-bit，PD=00（转换间隙
// 掉电、PENIRQ 使能）。本驱动轮询，不接 T_IRQ。
//
// 抗抖动策略（电阻屏按下/抬起瞬间 ADC 抖动可达上千）：
//   - 每轮 5 次连续采样（2MHz 下整轮仅约 0.2ms，不需要 vTaskDelay）
//   - 仅保留落在有效触力区间 [120,3950] 的样本，≥3/5 有效才判为按下
//   - 对有效样本排序取中值作为坐标，并记录极差(spread)用于诊断噪声
#include "display_service/xpt2046.h"

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "xpt2046";

static spi_device_handle_t s_dev = nullptr;
static xpt2046_cal_t s_cal = {
    .swap_xy   = false,
    .invert_x  = true,
    .invert_y  = false,
    .x_min     = 300,
    .x_max     = 3800,
    .y_min     = 200,
    .y_max     = 3900,
};

// 采样参数
static constexpr int      kSamples = 5;     // 每轮采样次数
static constexpr int      kMinValid = 3;    // 至少多少个有效样本才算按下
static constexpr uint16_t kRawLo   = 120;   // 按下时 ADC 合理下限
static constexpr uint16_t kRawHi   = 3950;  // 按下时 ADC 合理上限（抬起约 4095）

// 最近一次中值采样结果（供映射层读取）
static uint16_t s_med_x = 0, s_med_y = 0;
static uint16_t s_spread_x = 0, s_spread_y = 0;

extern "C" esp_err_t xpt2046_init(spi_host_device_t host, int cs_gpio)
{
    if (s_dev) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = 2 * 1000 * 1000;  // 2 MHz
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = cs_gpio;
    dev_cfg.queue_size = 1;
    dev_cfg.pre_cb = nullptr;
    dev_cfg.post_cb = nullptr;

    esp_err_t err = spi_bus_add_device(host, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "init CS=%d cal: x[%u-%u] y[%u-%u] inv=(%d,%d) swap=%d",
             cs_gpio, s_cal.x_min, s_cal.x_max, s_cal.y_min, s_cal.y_max,
             s_cal.invert_x, s_cal.invert_y, s_cal.swap_xy);
    return ESP_OK;
}

extern "C" void xpt2046_set_calibration(const xpt2046_cal_t* cal)
{
    if (!cal) return;
    s_cal = *cal;
    ESP_LOGI(TAG, "calibration: x[%u-%u] y[%u-%u] inv=(%d,%d) swap=%d",
             s_cal.x_min, s_cal.x_max, s_cal.y_min, s_cal.y_max,
             s_cal.invert_x, s_cal.invert_y, s_cal.swap_xy);
}

// 单次读取指定通道的 ADC 值 (12-bit)：命令字节后 16 个时钟返回
static uint16_t read_channel(uint8_t cmd)
{
    uint8_t tx_buf[3] = { cmd, 0x00, 0x00 };
    uint8_t rx_buf[3] = { 0, 0, 0 };

    spi_transaction_t t = {};
    t.length = 24;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;

    spi_device_transmit(s_dev, &t);
    return ((uint16_t)(rx_buf[1] & 0x0F) << 8) | rx_buf[2];
}

// 插入排序后取中值和极差
static void SortMedian(uint16_t* a, int n, uint16_t* median, uint16_t* spread)
{
    for (int i = 1; i < n; ++i) {
        uint16_t key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; --j; }
        a[j + 1] = key;
    }
    *median = a[n / 2];
    *spread = (uint16_t)(a[n - 1] - a[0]);
}

extern "C" esp_err_t xpt2046_read_xy(uint16_t* x, uint16_t* y, bool* pressed)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    // 通道上电预热两次，丢弃（XPT2046 差分模式首次转换不稳定）
    read_channel(0x90);
    read_channel(0xD0);

    uint16_t vx[kSamples], vy[kSamples];
    int n = 0;
    for (int i = 0; i < kSamples; ++i) {
        uint16_t ry = read_channel(0xD0);
        uint16_t rx = read_channel(0x90);
        if (rx > kRawLo && rx < kRawHi && ry > kRawLo && ry < kRawHi) {
            vx[n] = rx;
            vy[n] = ry;
            ++n;
        }
    }

    static bool s_was_pressed = false;
    static int64_t s_press_start_us = 0;
    static int64_t s_last_hold_log_us = 0;
    static int64_t s_last_idle_log_us = 0;
    const int64_t now_us = esp_timer_get_time();

    bool is_pressed = (n >= kMinValid);

    if (n > 0) {
        SortMedian(vx, n, &s_med_x, &s_spread_x);
        SortMedian(vy, n, &s_med_y, &s_spread_y);
    }

    if (is_pressed) {
        *x = s_med_x;
        *y = s_med_y;
        if (!s_was_pressed) {
            // 按下沿：打印中值/极差/有效数（极差大说明屏或线噪声大）
            ESP_LOGI(TAG, "PRESS raw=(%u,%u) spread=(%u,%u) valid=%d/%d",
                     s_med_x, s_med_y, s_spread_x, s_spread_y, n, kSamples);
            s_press_start_us = now_us;
            s_last_hold_log_us = now_us;
        } else if (now_us - s_last_hold_log_us > 200 * 1000) {
            ESP_LOGI(TAG, "hold  raw=(%u,%u) spread=(%u,%u)",
                     s_med_x, s_med_y, s_spread_x, s_spread_y);
            s_last_hold_log_us = now_us;
        }
        s_last_idle_log_us = now_us;
    } else {
        *x = s_med_x;
        *y = s_med_y;
        if (s_was_pressed) {
            ESP_LOGI(TAG, "RELEASE after %lld ms, last raw=(%u,%u) valid=%d/%d",
                     (long long)((now_us - s_press_start_us) / 1000),
                     s_med_x, s_med_y, n, kSamples);
        } else if (now_us - s_last_idle_log_us > 3 * 1000 * 1000) {
            // 空闲心跳：读值应约 4095 不动；恒定 0/4095 且按下无变化 = 接线问题
            uint16_t px = read_channel(0x90);
            uint16_t py = read_channel(0xD0);
            ESP_LOGI(TAG, "idle probe raw=(%u,%u)", px, py);
            s_last_idle_log_us = now_us;
        }
    }

    s_was_pressed = is_pressed;
    *pressed = is_pressed;
    return ESP_OK;
}

extern "C" esp_err_t xpt2046_read_screen_xy(int screen_width, int screen_height,
                                            int16_t* screen_x, int16_t* screen_y,
                                            bool* pressed)
{
    uint16_t raw_x = 0, raw_y = 0;
    bool is_pressed = false;

    esp_err_t err = xpt2046_read_xy(&raw_x, &raw_y, &is_pressed);
    if (err != ESP_OK) return err;

    if (!is_pressed) {
        *pressed = false;
        // 注意：这里不再写 (-1,-1)。LVGL 判定 CLICK 需要抬起坐标与按下坐标
        // 距离 < scroll_limit(默认10px)，上报 (-1,-1) 会让所有点击被判为滑动。
        // 调用方（IndevRead）负责在抬起时继续上报最后一次有效坐标。
        return ESP_OK;
    }

    // 线性映射：允许少量越界（±5%）后再钳制到屏幕边缘，避免噪声被直接钉在 0/239
    auto map_axis = [](uint16_t raw, uint16_t lo, uint16_t hi, int extent) -> int {
        if (hi <= lo) return 0;
        float n = (float)(raw - lo) / (float)(hi - lo);
        if (n < -0.05f) n = -0.05f;
        if (n > 1.05f) n = 1.05f;
        int v = (int)(n * (extent - 1) + (n >= 0 ? 0.5f : -0.5f));
        if (v < 0) v = 0;
        if (v > extent - 1) v = extent - 1;
        return v;
    };

    int fx = map_axis(raw_x, s_cal.x_min, s_cal.x_max, screen_width);
    int fy = map_axis(raw_y, s_cal.y_min, s_cal.y_max, screen_height);

    if (s_cal.invert_x) fx = (screen_width - 1) - fx;
    if (s_cal.invert_y) fy = (screen_height - 1) - fy;
    if (s_cal.swap_xy) {
        int t = fx; fx = fy; fy = t;
    }

    *screen_x = static_cast<int16_t>(fx);
    *screen_y = static_cast<int16_t>(fy);
    *pressed = true;

    static int64_t s_last_map_log_us = 0;
    const int64_t now2 = esp_timer_get_time();
    if (now2 - s_last_map_log_us > 200 * 1000) {
        ESP_LOGI(TAG, "map   screen=(%d,%d) raw=(%u,%u) inv=(%d,%d) swap=%d",
                 *screen_x, *screen_y, raw_x, raw_y,
                 s_cal.invert_x, s_cal.invert_y, s_cal.swap_xy);
        s_last_map_log_us = now2;
    }

    return ESP_OK;
}
