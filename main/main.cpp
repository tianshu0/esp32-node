// esp32-node 装配入口：只做与硬件无关的系统初始化，再交给板型装配层
//
// 固定职责链（所有板型相同）：
//   app_config      -> 加载 NVS（node_id / I2C 引脚 / 上报间隔 / hub_id）
//   sensor_registry -> 传感器能力注册表（具体传感器由板型装配层登记）
//   data_pipeline   -> 持有事件基，配对后定时采集 -> 打包 -> 投递 kEventSample
//   BoardAssemble   -> 板型相关：总线 / 传感器 / 显示屏（Kconfig 选中的 boards/<name>）
//   power_manager   -> 依据 app_config.power_save 决定是否启用自动 light sleep
//   wifi_portal     -> SoftAP 网页配网（本设备不启用 BLE，走 WiFi 配网）
//
// 新增传感器或显示屏不需要改本文件，见 main/Kconfig.projbuild 与 README「硬件扩展」。
#include "esp_event.h"
#include "esp_log.h"

#include "app_config/AppConfig.hpp"
#include "sensor_registry/SensorRegistry.hpp"
#include "data_pipeline/DataPipeline.hpp"
#include "power_manager/PowerManager.hpp"
#include "wifi_portal/WifiPortal.hpp"
#include "Board.hpp"

#if CONFIG_BT_ENABLED
#include "nimble/nimble_port.h"
#include "ble_peripheral/BlePeripheral.hpp"
#endif

using namespace esp32node;

static const char* TAG = "esp32-node";

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 以下组件均以 static 存在：内部创建了任务/注册了事件回调，
    // 若作为栈对象在 app_main 返回时析构会造成悬挂指针，故常驻到系统重启。
    static AppConfig config;         // 配置存储（NVS 读写，内部初始化 NVS）
    // NVS 必须先于 Wi-Fi 初始化：phy 校准数据的读写要走 NVS，否则会报
    // "esp_phy_load_cal_data_from_nvs: NVS has not been initialized" 并退回全量校准。
    ESP_ERROR_CHECK(config.Init());

#if CONFIG_BT_ENABLED
    // NimBLE 协议栈初始化（host 必须先于 ble_peripheral 启动）
    ESP_ERROR_CHECK(nimble_port_init());
#endif

    static SensorRegistry registry;  // 能力注册表（板型层登记，pipeline/display 查询）
    ESP_ERROR_CHECK(registry.Init());

    // data_pipeline 持有事件基，display/automation/ble 订阅该事件基投递的采集数据
    static DataPipeline pipeline;    // 采集调度：配对后按间隔采集并打包投递
    ESP_ERROR_CHECK(pipeline.Init(&config, &registry));

#if CONFIG_BT_ENABLED
    static BlePeripheral ble;        // 广播 / GATT 服务端 / 握手 / 数据上报
    ESP_ERROR_CHECK(ble.Init(&config, &registry));
#endif

    // 板型装配：创建总线、勾选的传感器、选中的显示屏（屏为可选项，失败只告警）
    static NodeContext board_ctx;
    board_ctx.config = &config;
    board_ctx.registry = &registry;
#if CONFIG_BT_ENABLED
    board_ctx.ble = &ble;
#endif
    BoardAssemble(board_ctx);

    static PowerManager power;       // 低功耗（app_config.power_save 开关，默认关闭）
    power.Init(&config);

    static WifiPortal portal;        // SoftAP 网页配网：手机连 FanNode-XXXX 自动弹配置页
    ESP_ERROR_CHECK(portal.Init(&config, board_ctx.fan, &registry));

    ESP_LOGI(TAG, "esp32-node started: id=%s i2c(sda=%d scl=%d) sensors=%d display=%s",
             config.NodeId().c_str(), config.I2cSda(), config.I2cScl(),
             registry.Count(), board_ctx.display_present ? "on" : "off");
}
