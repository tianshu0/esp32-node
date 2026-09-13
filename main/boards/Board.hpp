// 板级装配契约：main.cpp 与具体硬件之间唯一的耦合点
//
// main.cpp 只负责与硬件无关的系统初始化（NVS / NimBLE / registry / pipeline /
// ble / power），然后调用 BoardAssemble()。具体「这块板挂什么总线、哪些传感器、
// 什么屏」由 Kconfig 选中的 main/boards/<name>/Board.cpp 实现。
//
// 新增板型：新建 main/boards/<name>/{Pins.hpp,Board.cpp} + 在 Kconfig.projbuild
// 增加一个 NODE_BOARD_* 选项 + 在 main/CMakeLists.txt 按宏加入该 Board.cpp。
#pragma once

namespace esp32node {

class AppConfig;
class SensorRegistry;
class BlePeripheral;

// 装配上下文：输入系统级组件指针，输出装配结果（供启动日志使用）
struct NodeContext {
    // ---- 输入（BoardAssemble 之前由 main.cpp 填好）----
    AppConfig* config = nullptr;
    SensorRegistry* registry = nullptr;
    BlePeripheral* ble = nullptr;

    // ---- 输出（BoardAssemble 内回填）----
    bool display_present = false;   // 是否成功点亮显示屏
};

// 由选中板型的 Board.cpp 实现：建总线 -> 实例化勾选的传感器 -> 装配选中的显示屏
void BoardAssemble(NodeContext& ctx);

} // namespace esp32node
