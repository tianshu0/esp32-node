// 板级装配契约：Application 与具体硬件之间唯一的耦合点
//
// Application 只负责与硬件无关的系统初始化，然后调用 Board.Assemble()。
// 具体「这个项目挂什么总线、哪些传感器、什么屏、用哪种链路」由 Kconfig 选中的
// main/boards/<name>/<Xxx>Board.cpp 实现——板子自己决定是 BLE 节点还是 WiFi 节点。
//
// 子系统：
//   - 总线：Board 直接暴露地址式原语（I2cWrite/I2cRead/I2cWriteRead/I2cProbe、
//     UartRead/UartWrite），接口里不出现 ESP-IDF 句柄；具体读写由各板在自己的
//     <Xxx>Board.cpp 里用 ESP-IDF 原语实现
//   - 显示：Board 在 Assemble 内创建板级显示对象（ThpDisplay / VocDisplay / FanDisplay），
//     失败则用 NoDisplay 兜底，保证 GetDisplay() 永不返回 nullptr
//   - 链路：Board 在 Assemble 内创建自己的 Link（BleLink / WifiLink），
//     Application 通过 GetLink() 启动并取状态，自身不再有任何传输分支
//
// 新增项目步骤：
//   1. 新建 main/boards/<name>/{config.h,config.json,<Xxx>Board.hpp,<Xxx>Board.cpp,<Xxx>Display.hpp,<Xxx>Display.cpp}
//   2. Kconfig.projbuild 的 NODE_PROJECT choice 加一个 config（select 内部符号）
//   3. main/CMakeLists.txt 加 elseif(CONFIG_NODE_PROJECT_<NAME>) 映射目录与文件名
#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "display/display.hpp"

namespace esp32node {

class AppConfig;
class SensorRegistry;
class Link;

// 装配上下文：输入系统级组件，输出装配结果（供启动日志使用）
struct NodeContext {
    // ---- 输入（Board::Assemble 之前由 Application 填好）----
    AppConfig* config = nullptr;
    SensorRegistry* registry = nullptr;

    // ---- 输出（Assemble 内回填）----
    bool display_present = false;   // 是否成功点亮显示屏
};

// 抽象基类：每块板是一个子类，GetBoard() 返回函数内 static 实例
class Board {
public:
    explicit Board(NodeContext& ctx) : ctx_(ctx) {}
    virtual ~Board() = default;

    // 板级装配：建总线 -> 实例化该项目的传感器 -> 装配显示屏/外设/链路
    virtual void Assemble() = 0;

    // 板名，用于启动日志（如 "s3_tft_fan"）
    virtual const char* Name() const = 0;

    // ---- 板载总线原语（地址式，屏蔽 ESP-IDF 句柄）----
    // 传感器驱动只依赖这几个方法访问总线、不持有任何句柄；具体读写由各板在
    // <Xxx>Board.cpp 里用 ESP-IDF 原语实现，只覆盖本板实际用到的那几个。
    virtual esp_err_t I2cWrite(uint8_t /*addr*/, const uint8_t* /*data*/, size_t /*len*/) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    virtual esp_err_t I2cRead(uint8_t /*addr*/, uint8_t* /*buf*/, size_t /*len*/) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // 写后重复起始再读（寄存器读取的原子形式）
    virtual esp_err_t I2cWriteRead(uint8_t /*addr*/, const uint8_t* /*w*/, size_t /*wlen*/,
                                   uint8_t* /*r*/, size_t /*rlen*/) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    virtual bool I2cProbe(uint8_t /*addr*/) { return false; }

    // UART 是点对点，无总线仲裁；timeout_ms 为 0 时非阻塞
    virtual int UartRead(uint8_t* /*buf*/, size_t /*len*/, uint32_t /*timeout_ms*/ = 0) {
        return 0;
    }
    virtual int UartWrite(const uint8_t* /*buf*/, size_t /*len*/) { return 0; }

    // 显示对象访问：Assemble 内创建（失败用 NoDisplay），生命周期与 Board 相同
    Display* GetDisplay() const { return display_; }

    // 本项目选定的无线链路（BleLink / WifiLink），Assemble 内填充
    Link* GetLink() const { return link_; }

    bool DisplayPresent() const { return ctx_.display_present; }

protected:
    NodeContext& ctx_;
    Display* display_ = nullptr;  // 子类 Assemble 内填充
    Link* link_ = nullptr;        // 子类 Assemble 内填充
};

// 由选中项目的 <Xxx>Board.cpp 提供：返回函数内 static 子类实例引用，
// 绑定到调用方传入的 ctx（Application 的成员，生命周期覆盖整个运行期）
Board& GetBoard(NodeContext& ctx);

} // namespace esp32node
