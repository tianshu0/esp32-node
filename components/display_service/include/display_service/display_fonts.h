// 中文字体声明（SimHei 子集，lv_font_conv 生成）
//
// 字库源文件：fonts/lv_font_zh16.c / lv_font_zh24.c
// 覆盖范围：ASCII 可见字符 + ° (U+00B0) + 本项目 UI 用到的简体中文子集
//
// 新增汉字后重新生成（在 components/display_service 目录下执行）。
// 注意三点：
//   1) 必须带 --lv-include lvgl.h，否则生成的 #include "lvgl/lvgl.h"
//      在 managed_components 的 lvgl 组件下找不到头文件；
//   2) 必须带 --no-compress，否则 bitmap_format=1，而 LVGL 未开启
//      LV_USE_FONT_COMPRESSED 时所有文字（含 ASCII）都不渲染；
//   3) 命令写成一整行，不要用反斜杠续行（会触发 -Wcomment）。
// 16px:
//   npx -y lv_font_conv --font C:/Windows/Fonts/simhei.ttf --size 16 --bpp 4 --format lvgl --lv-font-name lv_font_zh16 --lv-include lvgl.h --no-compress -r 0x20-0x7F -r 0xB0 --symbols <全部汉字写这里> -o fonts/lv_font_zh16.c
// 24px:
//   npx -y lv_font_conv --font C:/Windows/Fonts/simhei.ttf --size 24 --bpp 4 --format lvgl --lv-font-name lv_font_zh24 --lv-include lvgl.h --no-compress -r 0x20-0x7F -r 0xB0 --symbols <全部汉字写这里> -o fonts/lv_font_zh24.c
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(lv_font_zh16);
LV_FONT_DECLARE(lv_font_zh24);

#ifdef __cplusplus
} // extern "C"
#endif
