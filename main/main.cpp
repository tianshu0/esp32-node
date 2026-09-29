// esp32-node 启动入口：硬件无关的系统初始化与板级装配全部由 Application 统领。
// main.cpp 只剩三行：构造 Application -> Init -> Run。
#include "application.hpp"

#include "esp_check.h"

using namespace esp32node;

extern "C" void app_main(void)
{
    // 必须 static：内部组件创建了任务/注册了事件回调，若作为栈对象在
    // app_main 返回时析构会造成悬挂指针，故常驻到系统重启。
    static Application app;
    ESP_ERROR_CHECK(app.Init());
    app.Run();
}
