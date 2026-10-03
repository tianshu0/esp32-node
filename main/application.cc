#include "application.h"

#include "board.h"
#include "display.h"

#include <esp_log.h>

#define TAG "Application"

#define SCHEDULE_EVENT (1 << 0)

Application::Application()
{
    event_group_ = xEventGroupCreate();
}

Application::~Application()
{
    vEventGroupDelete(event_group_);
}

void Application::Initialize()
{
    ESP_LOGI(TAG, "Initializing application...");

    // 获取板级单例：构造函数内完成总线/显示屏等硬件初始化
    auto& board = Board::GetInstance();

    // 构建 UI（具体显示类实现；NoDisplay 时为空操作）
    auto display = board.GetDisplay();
    display->SetupUI();

    ESP_LOGI(TAG, "Application initialized");
}

void Application::Run()
{
    ESP_LOGI(TAG, "Running application...");

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_,
                                               SCHEDULE_EVENT,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);
        if (!(bits & SCHEDULE_EVENT)) {
            continue;
        }

        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(schedule_mutex_);
            callbacks.swap(schedules_);
        }
        for (auto& callback : callbacks) {
            callback();
        }
    }
}

void Application::Schedule(std::function<void()>&& callback)
{
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        schedules_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}
