// AHT20 温湿度传感器驱动（I2C）—— 独立可移植组件
//
// 仅依赖 ESP-IDF：driver/i2c_master.h + esp_timer.h
// 板级负责创建 i2c_master_bus_handle_t 并传入；本组件自行挂载设备句柄。
//
// - 7 位地址：0x38（固定）
// - 上电后等待 ≥40ms，发送校准命令 0xBE 0x08 0x00；状态字 bit3=1 表示已校准
// - 触发测量：0xAC 0x33 0x00，转换最长 80ms（状态字 bit7=0 表示完成）
// - 读回 7 字节：status + 20bit 湿度 + 20bit 温度 + CRC8（poly 0x31，覆盖前 6 字节）
//     RH = raw_h / 2^20 * 100 %；T = raw_t / 2^20 * 200 - 50 ℃
#pragma once

#include <cstdint>
#include <esp_err.h>
#include <driver/i2c_master.h>

class Aht20 {
public:
    static constexpr uint8_t kAddr = 0x38;

    // bus: 板级已初始化的 I2C 主总线句柄；addr: 器件 7 位地址（默认 0x38）
    Aht20(i2c_master_bus_handle_t bus, uint8_t addr = kAddr);
    ~Aht20();

    // 软复位 + 校准命令，等待校准完成。器件无应答/未校准返回非 OK。
    esp_err_t Init();

    // 单次采集：温度（℃）+ 相对湿度（%RH）。任一指针可传 nullptr 跳过。
    bool Read(float* temperature_c, float* humidity_pct);

private:
    esp_err_t Transmit(const uint8_t* data, size_t len);
    bool ReadStatus(uint8_t* status);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t addr_ = kAddr;
};
