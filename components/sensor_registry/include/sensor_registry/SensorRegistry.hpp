// sensor_registry 组件：节点自身传感器能力注册表
//
// 职责：
//   - sensor_driver 启动时把「类型 / 型号 / 数据格式 / 采集函数」登记进来
//   - data_pipeline 遍历注册表采集（不需要知道具体传感器型号）
//   - ble_peripheral 握手时取能力清单上报给 hub
//
// 设计要点：
//   - 用统一签名的函数指针（C 风格 vtable）而非抽象基类，保持 ESP-IDF 习惯
//   - 被动组件：不自建任务、不订阅事件，只提供纯数据操作
//   - 本组件不依赖任何其他业务组件，避免与 sensor_driver 形成循环依赖
#pragma once

#include <cstdint>
#include "esp_err.h"

namespace esp32node {

// 一次采集结果。
// type 字段由 SensorRegistry::ReadAll 统一填入（驱动不必重复写自己的类型），
// 驱动只负责填 ts_ms 与 values_json。
struct SensorReading {
    char type[24];           // 传感器类型标识，如 "temp_hum"
    int64_t ts_ms;           // 采集时刻（毫秒）
    char values_json[160];   // 值对象 JSON，如 {"temp":23.5,"humidity":65.2}
};

// 统一采集函数签名：成功返回 true 并填好 out
using SensorReadFn = bool (*)(void* ctx, SensorReading* out);

class SensorRegistry {
public:
    static constexpr int kMaxSensors = 4;

    SensorRegistry() = default;
    ~SensorRegistry() = default;

    esp_err_t Init();

    // 登记一个传感器（由 sensor_driver 在自身 Init 内调用）
    // format_json 形如 {"temp":"float","humidity":"float","unit":"C/%"}
    esp_err_t Register(const char* type, const char* model, const char* format_json,
                       SensorReadFn read, void* ctx);

    int Count() const { return count_; }
    const char* TypeAt(int index) const;

    // 遍历所有已注册传感器采集，返回成功读数个数（最多 max 个）
    int ReadAll(SensorReading* out, int max) const;

    // 能力清单片段（握手时由 ble_peripheral 组装进 hello_ack）
    const char* TypesJson() const { return types_json_; }    // ["temp_hum","pressure"]
    const char* DetailJson() const { return detail_json_; }  // [{"type":..,"model":..,"format":{..}}]

private:
    struct Entry {
        char type[24];
        char model[16];
        char format[80];
        SensorReadFn read = nullptr;
        void* ctx = nullptr;
    };

    void RebuildJson();

    Entry entries_[kMaxSensors];
    int count_ = 0;
    char types_json_[128] = "[]";
    char detail_json_[640] = "[]";
};

} // namespace esp32node
