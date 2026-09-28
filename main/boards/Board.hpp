// 板级装配契约：main.cpp 与具体硬件之间唯一的耦合点
//
// Application 只负责与硬件无关的系统初始化，然后调用 Board.Assemble()。
// 具体「这个项目挂什么总线、哪些传感器、什么屏」由 Kconfig 选中的
// main/boards/<name>/<Xxx>Board.cpp 实现。
//
// 新增项目步骤：
//   1. 新建 main/boards/<name>/{config.h,config.json,<Xxx>Board.hpp,<Xxx>Board.cpp}
//   2. Kconfig.projbuild 的 NODE_PROJECT choice 加一个 config（select 内部符号）
//   3. main/CMakeLists.txt 加 elseif(CONFIG_NODE_PROJECT_<NAME>) 映射目录与文件名
#pragma once

namespace esp32node {

class AppConfig;
class SensorRegistry;
class BlePeripheral;
class FanControl;

// 装配上下文：输入系统级组件指针，输出装配结果（供启动日志使用）
struct NodeContext {
    // ---- 输入（Board::Assemble 之前由 Application 填好）----
    AppConfig* config = nullptr;
    SensorRegistry* registry = nullptr;
    BlePeripheral* ble = nullptr;   // 仅 CONFIG_BT_ENABLED 时填入

    // ---- 输出（Assemble 内回填）----
    bool display_present = false;   // 是否成功点亮显示屏
    FanControl* fan = nullptr;      // 项目有 PWM 风扇时回填，供 wifi_portal 控制
};

// 抽象基类：每块板是一个子类，GetBoard() 返回函数内 static 实例
class Board {
public:
    explicit Board(NodeContext& ctx) : ctx_(ctx) {}
    virtual ~Board() = default;

    // 板级装配：建总线 -> 实例化该项目的传感器 -> 装配显示屏/风扇
    virtual void Assemble() = 0;

    // 板名，用于启动日志（如 "s3_tft_fan"）
    virtual const char* Name() const = 0;

    bool DisplayPresent() const { return ctx_.display_present; }
    FanControl* GetFan() const { return ctx_.fan; }

protected:
    NodeContext& ctx_;
};

// 由选中项目的 <Xxx>Board.cpp 提供：返回函数内 static 子类实例引用，
// 绑定到调用方传入的 ctx（Application 的成员，生命周期覆盖整个运行期）
Board& GetBoard(NodeContext& ctx);

} // namespace esp32node
