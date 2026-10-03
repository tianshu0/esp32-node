# fonts 组件：中文字库（SimHei 子集，lv_font_conv 生成）
#
# 独立可移植组件：任何使用 LVGL 的项目都可直接引用。
# 头文件：fonts/fonts.h（声明 lv_font_zh8/10/12/14/16/24）
# 源文件：src/lv_font_zh*.c
idf_component_register(
    SRCS
        "src/lv_font_zh8.c"
        "src/lv_font_zh10.c"
        "src/lv_font_zh12.c"
        "src/lv_font_zh14.c"
        "src/lv_font_zh16.c"
        "src/lv_font_zh24.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
