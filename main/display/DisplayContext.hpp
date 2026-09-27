// 显示装配上下文：板型装配层把「系统组件指针 + 选中的内容模板」交给显示驱动
//
// 显示驱动（DisplayDevice：什么屏）与内容模板（Screen：显示什么）正交：
// 同一个 Ssd1315Display 可以配 DashboardScreen，也可以配将来的 StatusScreen；
// 同一个 DashboardScreen 理论上也能搬到彩色屏驱动。二者在板型装配处组合。
//
// 生命周期契约：驱动 Start() 会按值拷贝本结构并长期持有其中指针（刷新任务持续
// 解引用）。所有被指对象（含 hw 指向的 HardwareContext 本体）必须用 static 或
// 等效方式常驻，不能是装配函数的栈对象。
#pragma once

namespace esp32node {

class AppConfig;
class SensorRegistry;
class BlePeripheral;
struct HardwareContext;
class Screen;

struct DisplayContext {
    AppConfig* config = nullptr;
    SensorRegistry* registry = nullptr;
    BlePeripheral* ble = nullptr;
    HardwareContext* hw = nullptr;
    Screen* screen = nullptr;
};

} // namespace esp32node
