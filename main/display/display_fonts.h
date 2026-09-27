// 中文字体声明（SimHei 子集，lv_font_conv 生成）
//
// 字库源文件：fonts/lv_font_zh16.c / lv_font_zh24.c
// 覆盖范围：ASCII 可见字符 + ° (U+00B0) + 本项目 UI 简体中文子集
//
// 重新生成步骤（用户在 PowerShell 里进入 main/display 目录执行）：
//   1) 必须带 --lv-include lvgl.h（否则 managed_components 下找不到头文件）；
//   2) 必须带 --no-compress（否则 LV_USE_FONT_COMPRESSED 关时全不渲染）；
//   3) 命令写成一整行（反斜杠续行会触发 -Wcomment）。
//   4) 新增汉字只需加到 --symbols 串末尾，lv_font_conv 自动去重。
// 16px 命令（一行）：
//   npx -y lv_font_conv --font C:/Windows/Fonts/simhei.ttf --size 16 --bpp 4 --format lvgl --lv-font-name lv_font_zh16 --lv-include lvgl.h --no-compress -r 0x20-0x7F -r 0xB0 --symbols 环境监测节点风扇控制湿度正常已开启已关闭运行中已停止偏高状态开关自动规则手动设置模式等待连接蓝牙网络时间日期周日快笼子温度启闭高湿正连网周期设待启关启闭运行中停止偏蓝牙等环测节温自规控备模—— -o fonts/lv_font_zh16.c
// 24px 命令（一行，同 --symbols 串）：
//   npx -y lv_font_conv --font C:/Windows/Fonts/simhei.ttf --size 24 --bpp 4 --format lvgl --lv-font-name lv_font_zh24 --lv-include lvgl.h --no-compress -r 0x20-0x7F -r 0xB0 --symbols 环境监测节点风扇控制湿度正常已开启已关闭运行中已停止偏高状态开关自动规则手动设置模式等待连接蓝牙网络时间日期周日快笼子温度启闭高湿正连网周期设待启关启闭运行中停止偏蓝牙等环测节温自规控备模—— -o fonts/lv_font_zh24.c
// 注：--symbols 里重复的字 lv_font_conv 会自动去重，多写无害。
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

#ifdef __cplusplus
} // extern "C"
#endif
