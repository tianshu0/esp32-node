// NoDisplay：空显示兜底（对标 xiaozhi NoDisplay）
//
// 用于屏未接/初始化失败的场景，所有方法空实现，Lock 恒成功。
// Board 构造函数检测屏失败后创建此对象，避免上层判空。
#pragma once

#include "Display.hpp"

namespace esp32node {

class NoDisplay : public Display {
private:
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}
};

} // namespace esp32node
