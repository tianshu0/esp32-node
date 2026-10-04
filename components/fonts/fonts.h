// 中文字体声明（SimHei 子集，lv_font_conv 生成）
//
// 字库源文件：fonts/lv_font_zh8/10/12/14/16/24.c
// 覆盖范围：ASCII 可见字符 + ° (U+00B0) + 本项目 UI 简体中文子集
//
// 重新生成（Node.js 环境，依赖 Windows 自带 SimHei 字体；从项目根目录）：
//   cd scripts
//   npm install          # 首次：安装 package.json 锁定的 lv_font_conv
//   node gen_fonts.js    # 或 npm run gen-fonts
//
// 注意事项：
//   1) 必须带 --lv-include lvgl.h（否则 managed_components 下找不到头文件）；
//   2) 必须带 --no-compress（否则 LV_USE_FONT_COMPRESSED 关时全不渲染）；
//   3) 新增汉字只需加到 scripts/gen_fonts.js 顶部 SYMS 串末尾，自动去重，
//      一次生成全部 6 个字号；脚本末尾会校验关键字形是否齐全。
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(lv_font_zh8);
LV_FONT_DECLARE(lv_font_zh10);
LV_FONT_DECLARE(lv_font_zh12);
LV_FONT_DECLARE(lv_font_zh14);
LV_FONT_DECLARE(lv_font_zh16);
LV_FONT_DECLARE(lv_font_zh24);

// 纯数字大字（0-9 . - / % C °，SimHei 18px）：传感器大数值显示用
LV_FONT_DECLARE(lv_font_num18);

#ifdef __cplusplus
} // extern "C"
#endif
