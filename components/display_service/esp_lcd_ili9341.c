// ILI9341 面板驱动实现（2.4" 240x320 SPI TFT）
//
// 结构完全对齐 ESP-IDF 内置 panel 驱动（esp_lcd_panel_st7789.c）：
// 一个 ili9341_panel_t 内嵌 esp_lcd_panel_t 作为基类，通过 __containerof 反查自身，
// 再把成员函数指针填进基类的函数表，交给 esp_lcd_panel_ops 通用 API 调用。
//
// 初始化序列基于 ILI9341 V2.4 datasheet + 社区验证的 AliExpress 2.4" TFT 模组配置。
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9341";

// 厂商初始化命令：cmd + 参数 + 参数长度 + 执行后延时
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_bytes;
    uint16_t delay_ms;
} ili9341_init_cmd_t;

// ILI9341 V2.4 标准初始化序列
// 关键点：
//   - 0x3A = 0x55 指定 RGB565 (16bpp)
//   - 0x36 (MADCTL) 的 bit3 (BGR) 控制 RGB 顺序
//   - 0x2A/0x2B 设定窗口（每次刷新由 draw_bitmap 重新下发）
static const ili9341_init_cmd_t kInitCmds[] = {
    {0x01, (const uint8_t[]){0x00}, 0, 10},   // SWRESET
    {0xCF, (const uint8_t[]){0x00, 0xD9, 0x30}, 3, 0},
    {0xED, (const uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (const uint8_t[]){0x85, 0x10, 0x78}, 3, 0},
    {0xCB, (const uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (const uint8_t[]){0x20}, 1, 0},
    {0xEA, (const uint8_t[]){0x00, 0x00}, 2, 0},
    {0xC0, (const uint8_t[]){0x23}, 1, 0},    // POWER1
    {0xC1, (const uint8_t[]){0x10}, 1, 0},    // POWER2
    {0xC5, (const uint8_t[]){0x3E, 0x28}, 2, 0}, // VCOM1/VCOM2
    {0xC7, (const uint8_t[]){0x86}, 1, 0},    // VCOM
    {0xB1, (const uint8_t[]){0x00, 0x18}, 2, 0}, // FRMCTR1
    {0xB6, (const uint8_t[]){0x08, 0x82, 0x27}, 3, 0}, // DISCTR
    {0xF2, (const uint8_t[]){0x00}, 1, 0},    // 3G gamma disable
    {0x26, (const uint8_t[]){0x01}, 1, 0},    // Gamma curve 3
    // Positive gamma correction
    {0xE0, (const uint8_t[]){0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                               0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15, 0},
    // Negative gamma correction
    {0xE1, (const uint8_t[]){0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                               0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15, 0},
    {0x11, (const uint8_t[]){0x00}, 0, 120}, // SLPOUT
    {0x29, (const uint8_t[]){0x00}, 0, 0},   // DISPON
};

// 面板私有数据
typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // MADCTL 当前值
} ili9341_panel_t;

static esp_err_t panel_ili9341_del(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9341_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9341_init(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9341_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_ili9341_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_ili9341_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_ili9341_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_ili9341_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_ili9341_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_ili9341(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    ili9341_panel_t *ili9341 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG,
                      err, TAG, "invalid argument");
    ili9341 = (ili9341_panel_t *)calloc(1, sizeof(ili9341_panel_t));
    ESP_GOTO_ON_FALSE(ili9341, ESP_ERR_NO_MEM, err, TAG, "no mem for ili9341 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST failed");
    }

    // RGB 顺序：MADCTL 的 BGR bit
    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        ili9341->madctl_val = 0x00;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        ili9341->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        ili9341->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        ili9341->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    ili9341->io = io;
    ili9341->reset_gpio_num = panel_dev_config->reset_gpio_num;
    ili9341->reset_level = panel_dev_config->flags.reset_active_high;

    ili9341->base.del = panel_ili9341_del;
    ili9341->base.reset = panel_ili9341_reset;
    ili9341->base.init = panel_ili9341_init;
    ili9341->base.draw_bitmap = panel_ili9341_draw_bitmap;
    ili9341->base.invert_color = panel_ili9341_invert_color;
    ili9341->base.set_gap = panel_ili9341_set_gap;
    ili9341->base.mirror = panel_ili9341_mirror;
    ili9341->base.swap_xy = panel_ili9341_swap_xy;
    ili9341->base.disp_on_off = panel_ili9341_disp_on_off;

    *ret_panel = &(ili9341->base);
    ESP_LOGD(TAG, "new ili9341 panel @%p", ili9341);
    return ESP_OK;

err:
    if (ili9341) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(ili9341);
    }
    return ret;
}

static esp_err_t panel_ili9341_del(esp_lcd_panel_t *panel)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    if (ili9341->reset_gpio_num >= 0) {
        gpio_reset_pin(ili9341->reset_gpio_num);
    }
    free(ili9341);
    return ESP_OK;
}

static esp_err_t panel_ili9341_reset(esp_lcd_panel_t *panel)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    if (ili9341->reset_gpio_num >= 0) {
        gpio_set_level(ili9341->reset_gpio_num, ili9341->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ili9341->reset_gpio_num, !ili9341->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, LCD_CMD_SWRESET, NULL, 0),
                            TAG, "send SWRESET failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_ili9341_init(esp_lcd_panel_t *panel)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    esp_lcd_panel_io_handle_t io = ili9341->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0),
                        TAG, "send SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    // 先下发基础 MADCTL/COLMOD
    uint8_t madctl = ili9341->madctl_val;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, &madctl, 1),
                        TAG, "send MADCTL failed");

    uint8_t colmod = (ili9341->fb_bits_per_pixel == 16) ? 0x55 : 0x66;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, &colmod, 1),
                        TAG, "send COLMOD failed");

    // 发送完整初始化序列
    const size_t cmds_num = sizeof(kInitCmds) / sizeof(kInitCmds[0]);
    for (size_t i = 0; i < cmds_num; i++) {
        const ili9341_init_cmd_t *cmd = &kInitCmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmd->cmd, cmd->data, cmd->data_bytes),
                            TAG, "send command %02Xh failed", cmd->cmd);
        if (cmd->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    ESP_LOGD(TAG, "send ili9341 init commands success");
    return ESP_OK;
}

static esp_err_t panel_ili9341_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start must be smaller than end");

    x_start += ili9341->x_gap;
    x_end   += ili9341->x_gap;
    y_start += ili9341->y_gap;
    y_end   += ili9341->y_gap;

    const uint8_t caset[] = {
        (uint8_t)(x_start >> 8), (uint8_t)(x_start & 0xFF),
        (uint8_t)((x_end - 1) >> 8), (uint8_t)((x_end - 1) & 0xFF),
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, LCD_CMD_CASET, caset, sizeof(caset)),
                        TAG, "send CASET failed");

    const uint8_t raset[] = {
        (uint8_t)(y_start >> 8), (uint8_t)(y_start & 0xFF),
        (uint8_t)((y_end - 1) >> 8), (uint8_t)((y_end - 1) & 0xFF),
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, LCD_CMD_RASET, raset, sizeof(raset)),
                        TAG, "send RASET failed");

    const size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * ili9341->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(ili9341->io, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_ili9341_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    const int cmd = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, cmd, NULL, 0),
                        TAG, "send invert color failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    if (mirror_x) {
        ili9341->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        ili9341->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        ili9341->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        ili9341->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, LCD_CMD_MADCTL,
                                                  &ili9341->madctl_val, 1),
                        TAG, "send MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    if (swap_axes) {
        ili9341->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        ili9341->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, LCD_CMD_MADCTL,
                                                  &ili9341->madctl_val, 1),
                        TAG, "send MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_ili9341_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    ili9341->x_gap = x_gap;
    ili9341->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_ili9341_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    ili9341_panel_t *ili9341 = __containerof(panel, ili9341_panel_t, base);
    const int cmd = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(ili9341->io, cmd, NULL, 0),
                        TAG, "send display on/off failed");
    return ESP_OK;
}
