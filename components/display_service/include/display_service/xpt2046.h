// XPT2046 电阻式触摸控制器驱动（SPI，本板使用独立 SPI3_HOST 总线）
//
// 工作流程：
//   1. 主机拉低 T_CS
//   2. 发送 8-bit 命令（通道 + 分辨率 + 电源）
//   3. 主机读取 12-bit ADC 返回值（两次 SPI 事务）
//   4. 主机拉高 T_CS
//
// XPT2046 命令格式（8-bit）：
//   bit7-4: 通道 (1001=X轴, 1101=Y轴)
//   bit3-2: 分辨率 (01=12-bit)
//   bit1:   电源模式 (1=ADC+驱动器全开启)
//   bit0:   0 (无操作)
//
// 本驱动使用 xpt2046_read_xy() 获取一组经过滤波的坐标，
// 配合 LVGL 的 lv_indev（输入设备）使用。
#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 XPT2046
//   host:     共享的 SPI 总线主机（SPI2_HOST = (spi_host_device_t)2）
//   cs_gpio:  XPT2046 的片选 GPIO
// 调用后 host 上会新增一个 XPT2046 设备
esp_err_t xpt2046_init(spi_host_device_t host, int cs_gpio);

// 读取触摸状态 + X/Y 坐标
//   x, y: 输出 (0-4095, 原始 ADC 值)
//   pressed: true = 正在触摸
// 返回值: ESP_OK 读取成功
esp_err_t xpt2046_read_xy(uint16_t* x, uint16_t* y, bool* pressed);

// 读取触摸状态 + 校准后的屏幕坐标
//   screen_x, screen_y: 输出 (0 到屏幕宽度/高度)
//   pressed: true = 正在触摸
// 校准参数在屏幕初始化时调用 xpt2046_set_calibration() 设置
esp_err_t xpt2046_read_screen_xy(int screen_width, int screen_height,
                                 int16_t* screen_x, int16_t* screen_y,
                                 bool* pressed);

// 设置校准参数（屏幕显示方向变换 + 原始 ADC 范围）
// 默认值：不旋转，X 范围 300-3800，Y 范围 200-3900（AliExpress 2.4" TFT 模组常见范围）
typedef struct {
    // 屏幕方向: 0=竖屏(portrait), 1=横屏(landscape, swap x/y)
    bool swap_xy;
    bool invert_x;
    bool invert_y;

    // XPT2046 原始 ADC 范围（4 点校准或 2 点 min/max 校准）
    uint16_t x_min, x_max;  // 触摸板有效 X 范围
    uint16_t y_min, y_max;  // 触摸板有效 Y 范围
} xpt2046_cal_t;

void xpt2046_set_calibration(const xpt2046_cal_t* cal);

#ifdef __cplusplus
} // extern "C"
#endif
