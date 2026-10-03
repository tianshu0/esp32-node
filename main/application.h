#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <functional>
#include <mutex>
#include <vector>

class Application {

public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();

    void Run();

     /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

private:
    Application();
    ~Application();

    EventGroupHandle_t event_group_ = nullptr;

    std::mutex schedule_mutex_;
    std::vector<std::function<void()>> schedules_;
};
