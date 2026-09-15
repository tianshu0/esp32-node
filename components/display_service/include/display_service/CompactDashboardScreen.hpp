// CompactDashboardScreen：128x32 单色 OLED 的紧凑仪表盘内容模板
//
// 与 DashboardScreen（128x64）思路一致：依据 SensorRegistry 字段描述符自动生成行，
// 不认识具体传感器。但 32px 高度只能容纳页眉 + 2 行数据，每行 2 列（共 4 个字段）。
// 字段数 >4 时按页循环（每页 4 个，3 秒切换），确保所有字段都能被看到。
#pragma once

#include "display_service/Screen.hpp"

namespace esp32node {

class CompactDashboardScreen : public Screen {
public:
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 32;
    static constexpr int kCells = 4;          // 2 行 x 2 列
    static constexpr uint32_t kPageMs = 3000; // 多页时切换周期

    void Build(lv_obj_t* root, const char* node_id,
               const SensorRegistry& registry) override;
    void SetStatus(const char* status) override;
    void Update(const SensorReading* samples, int count,
                const SensorRegistry& registry) override;

private:
    struct Cell {
        char sensor_type[24];
        char key[12];
        char unit[8];
        uint8_t decimals = 1;
        lv_obj_t* label = nullptr;   // "标签 值 单位" 合并到一个 label，省对象
        char cache[24] = {};
    };

    lv_obj_t* status_label_ = nullptr;
    char status_cache_[12] = {};
    Cell cells_[kCells];
    int total_fields_ = 0;
    uint32_t last_page_switch_ms_ = 0;
    int page_ = 0;
};

} // namespace esp32node
