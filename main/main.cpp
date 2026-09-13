// esp32-node 装配入口：只做与硬件无关的系统初始化，再交给板型装配层
//
// 固定职责链（所有板型相同）：
//   app_config      -> 加载 NVS（node_id / I2C 引脚 / 上报间隔 / hub_id）
//   nimble          -> NimBLE 协议栈
//   sensor_registry -> 传感器能力注册表（具体传感器由板型装配层登记）
//   data_pipeline   -> 持有事件基，配对后定时采集 -> 打包 -> 投递 kEventSample
//   ble_peripheral  -> 广播 + GATT 服务端 + 握手，订阅 kEventSample 做 Notify 上报
//   BoardAssemble   -> 板型相关：总线 / 传感器 / 显示屏（Kconfig 选中的 boards/<name>）
//   power_manager   -> 依据 app_config.power_save 决定是否启用自动 light sleep
//
// 新增传感器或显示屏不需要改本文件，见 main/Kconfig.projbuild 与 README「硬件扩展」。
#include "esp_event.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "data_pipeline/DataPipeline.hpp"
#include "ble_peripheral/BlePeripheral.hpp"
#include "power_manager/PowerManager.hpp"
#include "Board.hpp"

using namespace esp32node;

static const char* TAG = "esp32-node";

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 以下组件均以 static 存在：内部创建了任务/注册了事件回调，
    // 若作为栈对象在 app_main 返回时析构会造成悬挂指针，故常驻到系统重启。
    static AppConfig config;         // 配置存储（NVS 读写，内部初始化 NVS）
    // NVS 必须先于 BT/Wi-Fi 初始化：phy 校准数据的读写要走 NVS，否则会报
    // "esp_phy_load_cal_data_from_nvs: NVS has not been initialized" 并退回全量校准，
    // 同时 BLE 的 IRK/bond 存储也不可用（Failed to restore IRKs）。
    ESP_ERROR_CHECK(config.Init());

    // NimBLE 协议栈初始化（host 必须先于 ble_peripheral 启动）
    ESP_ERROR_CHECK(nimble_port_init());

    static SensorRegistry registry;  // 能力注册表（板型层登记，pipeline/ble/display 查询）
    ESP_ERROR_CHECK(registry.Init());

    // data_pipeline 先于 ble_peripheral 初始化：
    // 前者持有事件基，后者订阅该事件基投递的采集数据
    static DataPipeline pipeline;    // 采集调度：配对后按间隔采集并打包投递
    ESP_ERROR_CHECK(pipeline.Init(&config, &registry));

    static BlePeripheral ble;        // 广播 / GATT 服务端 / 握手 / 数据上报
    ESP_ERROR_CHECK(ble.Init(&config, &registry));

    // 板型装配：创建总线、勾选的传感器、选中的显示屏（屏为可选项，失败只告警）
    static NodeContext board_ctx;
    board_ctx.config = &config;
    board_ctx.registry = &registry;
    board_ctx.ble = &ble;
    BoardAssemble(board_ctx);

    static PowerManager power;       // 低功耗（app_config.power_save 开关，默认关闭）
    power.Init(&config);

    ESP_LOGI(TAG, "esp32-node started: id=%s i2c(sda=%d scl=%d) sensors=%d display=%s",
             config.NodeId().c_str(), config.I2cSda(), config.I2cScl(),
             registry.Count(), board_ctx.display_present ? "on" : "off");
}
