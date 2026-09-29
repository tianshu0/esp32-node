// 显示框架基类（对标 esp32-xiaozhi display.h）
//
// 设计原则：
//   - 被动接口：Application 推数据（SetStatus / UpdateSamples / ShowNotification），
//     不主动拉传感器
//   - 默认空实现：子类按需重写，基类方法只打日志
//   - 构造注入：硬件句柄（io/panel/配置）在板装配层创建后传入构造函数
//   - 线程安全：子类用 DisplayLockGuard RAII 保护 LVGL 操作
//
// 生命周期：作为 Board 成员或 static 对象常驻到系统重启。
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sensor_registry/sensor_registry.hpp"

namespace esp32node {

class Display {
public:
    Display() = default;
    virtual ~Display() = default;

    // 状态栏文本（如 "ADV"/"CONN"/"PAIRED" 或 WiFi 连接状态）
    virtual void SetStatus(const char* status) {
        ESP_LOGD("display", "SetStatus: %s", status);
    }

    // 周期推送传感器采样结果（Application 1s 任务调用）
    // samples: 数组指针；count: 有效条目数
    virtual void UpdateSamples(const SensorReading* samples, int count) {
        ESP_LOGD("display", "UpdateSamples: %d entries", count);
    }

    // 临时通知（如 BLE 配对成功），duration_ms 后自动恢复
    virtual void ShowNotification(const char* text, int duration_ms = 3000) {
        ESP_LOGD("display", "Notification: %s (%dms)", text, duration_ms);
    }

    // 背光控制（无背光设备空实现）
    virtual void SetBacklight(bool on) {
        ESP_LOGD("display", "SetBacklight: %s", on ? "on" : "off");
    }

    // 分辨率查询（子类构造时填充）
    int width() const { return width_; }
    int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;

    // 友元 RAII 锁：子类实现 Lock/Unlock（通常是 lvgl_port_lock/unlock）
    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

// RAII LVGL 锁（对标 xiaozhi DisplayLockGuard）
class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_->Lock(30000)) {
        if (!locked_) {
            ESP_LOGE("display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

    explicit operator bool() const { return locked_; }

private:
    Display* display_;
    bool locked_;
};

} // namespace esp32node
