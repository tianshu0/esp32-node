// DashboardScreen：通用传感器仪表盘内容模板
//
// 不认识任何具体传感器：Build() 时遍历 SensorRegistry 的字段描述符自动生成行
// （标签左、数值右对齐），Update() 时按 传感器type + 字段key 从采集 JSON 取值。
// 新增任何传感器，只要在驱动 Start() 里登记了 fields，本屏自动出现对应行，无需改代码。
//
// 128x64 单色 OLED 上按行数自适应字号：≤3 行 14px（默认 T/H/P），
// 4 行 12px，≥5 行 10px；超过屏幕容量的字段丢弃并告警（将来按钮翻页再扩展）。
#pragma once

#include "display_service/Screen.hpp"

namespace esp32node {

class DashboardScreen : public Screen {
public:
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;
    static constexpr int kMaxRows = 6;

    void Build(lv_obj_t* root, const char* node_id,
               const SensorRegistry& registry) override;
    void SetStatus(const char* status) override;
    void Update(const SensorReading* samples, int count,
                const SensorRegistry& registry) override;

private:
    struct Row {
        char sensor_type[24];   // 该行属于哪个传感器（匹配 SensorReading.type）
        char key[12];           // values_json 中的字段名
        char unit[8];           // 显示单位后缀
        uint8_t decimals = 1;
        lv_obj_t* value = nullptr;
        char cache[20] = {};    // 文本去重，避免无谓的整屏 I2C 重绘
    };

    lv_obj_t* status_label_ = nullptr;
    char status_cache_[12] = {};
    Row rows_[kMaxRows];
    int row_count_ = 0;
};

} // namespace esp32node
