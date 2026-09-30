// Application：统领 app_main 的全部系统初始化与板级装配流程（对标 esp32-xiaozhi）
//
// 职责：把与硬件无关的系统初始化（事件循环 / NVS / registry / power）和
// 板级装配（GetBoard().Assemble() -> Link.Start()）串成一条链，
// main.cpp 只剩「构造 -> Init -> Run」三行。
//
// 传输无关：BLE / WiFi 的选择由板装配层完成（Board::GetLink()），
// 本文件不含任何 CONFIG_BT_ENABLED 分支。
//
// 生命周期：作为 app_main 内的 static 对象存在，常驻到系统重启。
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config/app_config.hpp"
#include "sensor_registry/sensor_registry.hpp"
#include "power_manager/power_manager.hpp"
#include "boards/board.hpp"

namespace esp32node {

class Application {
public:
    // 系统初始化 + 板级装配：事件循环 -> config -> registry ->
    // Board 装配(总线/传感器/屏/链路) -> link 启动 -> power
    esp_err_t Init();

    // 启动日志（项目名 / 节点 ID / I2C 引脚 / 传感器数 / 显示屏状态）
    void Run();

private:
    // 1s 周期采集任务：读取传感器并推给显示（UpdateSamples/SetStatus）
    static void SensorTask(void* arg);
    void UpdateDisplay();

    AppConfig config_;         // 配置存储（NVS 读写，内部初始化 NVS）
    SensorRegistry registry_;  // 能力注册表（板级装配层登记传感器）
    PowerManager power_;       // 低功耗（app_config.power_save 开关，默认关闭）

    NodeContext board_ctx_;    // 装配上下文：输入 config/registry，输出 display_present
    Board* board_ = nullptr;   // 指向 GetBoard() 的 static 实例（Init 内填充）

    TaskHandle_t sensor_task_ = nullptr;  // 1s 采集任务句柄
};

} // namespace esp32node
