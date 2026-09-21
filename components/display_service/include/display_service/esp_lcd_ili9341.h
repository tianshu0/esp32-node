// ILI9341 面板驱动头文件
// 与 esp_lcd_new_panel_st7789 等保持相同的 C 接口
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_new_panel_ili9341(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
} // extern "C"
#endif
