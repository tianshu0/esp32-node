// 显示内容模板基类：只管「显示什么」——在给定 LVGL 容器内构建 UI 并周期更新
//
// 与具体面板无关（拿到 root 容器和注册表就能工作）。
// Build() 在 LVGL 锁内、面板初始化完成后调用一次；
// SetStatus()/Update() 由显示驱动任务每秒在 LVGL 锁内调用。
#pragma once

#include "lvgl.h"
#include "sensor_registry/SensorRegistry.hpp"

namespace esp32node {

class Screen {
public:
    virtual ~Screen() = default;

    // 构建静态布局（页眉/分隔线/字段行）。字段行依据 registry 的字段描述符自动生成。
    virtual void Build(lv_obj_t* root, const char* node_id,
                       const SensorRegistry& registry) = 0;

    // 更新右上角连接状态（ADV / CONN / PAIRED）
    virtual void SetStatus(const char* status) = 0;

    // 更新数值行：samples 为本周期采集结果（类型在 SensorReading.type），
    // 模板按字段 key 从 values_json 取值；读不到的字段显示 "--"。
    virtual void Update(const SensorReading* samples, int count,
                        const SensorRegistry& registry) = 0;
};

} // namespace esp32node
