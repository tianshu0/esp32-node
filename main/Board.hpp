// 项目装配契约：main.cpp 与具体硬件之间唯一的耦合点
//
// main.cpp 只负责与硬件无关的系统初始化（NVS / registry / pipeline /
// power / wifi），然后调用 BoardAssemble()。具体「这个项目挂什么总线、哪些传感器、
// 什么屏」由 Kconfig 选中的 main/projects/<name>/Board.cpp 实现。
//
// 「项目即宏」机制（对标 esp32-xiaozhi 的板型选择）：
//   在 Kconfig.projbuild 的 NODE_PROJECT choice 选一个项目宏，宏通过 select
//   自动锁定屏/传感器/触摸等内部开关；每个项目目录自包含：
//     config.h    板级编译期常量（引脚/通道，仅本项目 Board.cpp include）
//     config.json 构建元数据（target 芯片 + sdkconfig 追加项，供 release.py）
//     Board.cpp   装配实现
//
// 新增项目：
//   1. 新建 main/projects/<name>/{config.h,config.json,Board.cpp}
//   2. Kconfig.projbuild 的 NODE_PROJECT choice 加一个 config（select 内部符号）
//   3. main/CMakeLists.txt 加 elseif(CONFIG_NODE_PROJECT_<NAME>) 映射目录
#pragma once

namespace esp32node {

class AppConfig;
class SensorRegistry;
class BlePeripheral;
class FanControl;

// 装配上下文：输入系统级组件指针，输出装配结果（供启动日志使用）
struct NodeContext {
    // ---- 输入（BoardAssemble 之前由 main.cpp 填好）----
    AppConfig* config = nullptr;
    SensorRegistry* registry = nullptr;
    BlePeripheral* ble = nullptr;   // 仅 CONFIG_BT_ENABLED 时由 main.cpp 填入

    // ---- 输出（BoardAssemble 内回填）----
    bool display_present = false;   // 是否成功点亮显示屏
    FanControl* fan = nullptr;      // 项目有 PWM 风扇时回填（如 s3_tft_fan），供网页配网控制
};

// 由选中项目的 Board.cpp 实现：建总线 -> 实例化该项目的传感器 -> 装配显示屏
void BoardAssemble(NodeContext& ctx);

} // namespace esp32node
