// esp32-node 装配入口：只做系统初始化 + 组件组合，业务状态机在各组件 Init 内部
//
// 职责链：
//   app_config      -> 加载 NVS（node_id / I2C 引脚 / 上报间隔 / hub_id）
//   sensor_driver   -> 初始化 I2C 总线 + SHT3X / BMP180，把能力登记到 sensor_registry
//   data_pipeline   -> 持有事件基，配对后定时采集 -> 打包 -> 投递 kEventSample
//   ble_peripheral  -> 广播 + GATT 服务端 + 握手，订阅 kEventSample 做 Notify 上报
//                     握手/断线时投递 kEventPaired / kEventUnpaired
//   power_manager   -> 依据 app_config.power_save 决定是否启用自动 light sleep
#include "esp_event.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "sensor_driver/SensorDriver.hpp"
#include "data_pipeline/DataPipeline.hpp"
#include "ble_peripheral/BlePeripheral.hpp"
#include "power_manager/PowerManager.hpp"

// 各组件类均定义于 esp32node 命名空间，装配入口统一引入
using namespace esp32node;

static const char* TAG = "esp32-node";

extern "C" void app_main(void)
{
    // 注意：NVS 初始化（含损坏擦除自愈）由 AppConfig::Init() 统一完成，
    // 这里不再裸调 nvs_flash_init()，否则分区表变动后旧数据会直接 abort。
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // NimBLE 协议栈初始化（host 必须先于 ble_peripheral 启动）
    ESP_ERROR_CHECK(nimble_port_init());

    // 以下组件均以 static 存在：内部创建了任务/注册了事件回调，
    // 若作为栈对象在 app_main 返回时析构会造成悬挂指针，故常驻到系统重启。
    static AppConfig config;         // 配置存储（NVS 读写，内部初始化 NVS）
    ESP_ERROR_CHECK(config.Init());

    static SensorRegistry registry;  // 能力注册表（driver 登记，pipeline/ble 查询）
    ESP_ERROR_CHECK(registry.Init());

    static SensorDriver driver;      // I2C 总线 + SHT3X（温湿度）+ BMP180（气压/海拔）
    ESP_ERROR_CHECK(driver.Init(&config, &registry));

    // data_pipeline 先于 ble_peripheral 初始化：
    // 前者持有事件基，后者订阅该事件基投递的采集数据
    static DataPipeline pipeline;    // 采集调度：配对后按间隔采集并打包投递
    ESP_ERROR_CHECK(pipeline.Init(&config, &registry));

    static BlePeripheral ble;        // 广播 / GATT 服务端 / 握手 / 数据上报
    ESP_ERROR_CHECK(ble.Init(&config, &registry));

    static PowerManager power;       // 低功耗（app_config.power_save 开关，默认关闭）
    power.Init(&config);

    ESP_LOGI(TAG, "esp32-node started: id=%s i2c(sda=%d scl=%d) sensors=%d",
             config.NodeId().c_str(), config.I2cSda(), config.I2cScl(),
             registry.Count());
}
