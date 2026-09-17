// 21VOC（安信可五合一空气质量模块）驱动（UART）—— SensorDevice 插件实现
//
// 模块输出：TVOC / CH2O(甲醛) / eCO2 / 温度 / 湿度，UART 9600 8N1 连续上报。
// 帧格式（12 字节，大端）：
//   B0     0x2C          帧头
//   B1-2   TVOC          ug/m³（uint16）
//   B3-4   CH2O          ug/m³（uint16）
//   B5-6   eCO2          ppm（uint16）
//   B7-8   温度          0.1℃（int16，bit15 为符号位）
//   B9-10  湿度          0.1%RH（uint16）
//   B11    校验和        (-(B0~B10 累加)) & 0xFF，即累加和的补码（取反加一）
//
// 读取策略：模块持续发送帧，驱动维护一个滑动缓冲，每次 Read() 把串口里已到达的
// 字节喂入状态机，取最近一帧校验通过的数据返回（未收到新帧时返回上一帧缓存）。
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "sensors/SensorDevice.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "uart_bus/UartBus.hpp"

namespace esp32node {

class Voc21Sensor : public SensorDevice {
public:
    static constexpr uint8_t kFrameHeader = 0x2C;
    static constexpr size_t kFrameLen = 12;

    const char* Type() const override { return "air_quality"; }
    esp_err_t Start(HardwareContext& hw, AppConfig& config,
                    SensorRegistry& registry) override;

    // 采集最近一帧：TVOC(ug/m³) / CH2O(ug/m³) / eCO2(ppm) / 温度(℃) / 湿度(%RH)
    // 任一指针为 nullptr 表示跳过该字段。返回 false 表示尚无有效帧。
    bool Read(uint16_t* tvoc, uint16_t* ch2o, uint16_t* eco2,
              float* temperature_c, float* humidity_pct);

    static bool ReadThunk(void* ctx, SensorReading* out);

private:
    // 把串口收到的字节喂入解析状态机，找到完整有效帧时更新缓存
    void Feed(const uint8_t* data, size_t len);

    UartBus* uart_ = nullptr;

    // 滑动接收缓冲（最多保留一帧长度，便于从头重新对齐）
    uint8_t rx_buf_[kFrameLen] = {};
    size_t rx_len_ = 0;

    // 最近一帧有效读数
    bool has_valid_ = false;
    uint16_t tvoc_ = 0;
    uint16_t ch2o_ = 0;
    uint16_t eco2_ = 0;
    float temperature_c_ = 0.0f;
    float humidity_pct_ = 0.0f;

    // 调试：前几帧有效数据打印原始帧+解码值，用于核对字段映射
    int debug_dump_left_ = 3;
};

} // namespace esp32node
