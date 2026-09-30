// link：无线链路抽象（BLE / WiFi），由板装配层二选一
//
// 解决的问题：节点「用什么方式与外界通信」原本由 Application 用
// #if CONFIG_BT_ENABLED 决定，导致传输身份散落在 Kconfig + Application 两处，
// 且「当前连接状态」这种板的属性被写成共享逻辑里的编译期分支。
//
// 现在：Board 持有 Link*，Application 只调 Start() / StatusText()。
// 板子自己决定是 BLE 节点还是 WiFi 节点；将来一块板要 BLE+WiFi 组合，
// 只需新增一个 Link 实现（组合而非多继承），Application 与其它板零改动。
#pragma once

#include "esp_err.h"

namespace esp32node {

class Link {
public:
    virtual ~Link() = default;

    // 启动链路：内部完成协议栈初始化、服务注册与任务创建
    virtual esp_err_t Start() = 0;

    // 状态栏短文本（BLE: "ADV"/"CONN"/"PAIRED"；WiFi: "已连接"/"未连接"）
    virtual const char* StatusText() const = 0;
};

} // namespace esp32node
