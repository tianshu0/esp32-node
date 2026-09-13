# esp32-node

ESP32 **传感器节点**固件：外接各类传感器，通过蓝牙自动发现并配对 `esp32-hub` 中继，配对成功后按约定间隔采集传感器数据并通过 BLE GATT Notify 上报给中继，由中继转发到 MQTT broker。

```
传感器 ──GPIO/I2C/SPI──▶ esp32-node ──BLE──▶ esp32-hub ──MQTT──▶ esp32-broker
```

## 角色定位

- **传感器采集**：通过 GPIO/I2C/SPI 读取外接传感器，按传感器类型打包数据。
- **蓝牙配对**：作为 BLE 外设（Peripheral）广播，等待 `esp32-hub` 主动连接，握手协商后建立配对。
- **数据上报**：配对成功后按约定间隔采集并通过 GATT Notify 上报，断线自动重连。
- **动态注册**：节点上报自身支持的传感器类型，由中继转注册到 MQTT，PC 端按类型解析展示。

## 为什么是当前的项目结构

整体采用 **ESP-IDF 标准组件化结构**，核心思想沿用 `esp32-broker`/`esp32-hub`：「一个职责 = 一个组件 = 自包含生命周期」。

```
esp32-node/
├── main/                   # 装配入口：系统初始化 + 组件组合，不含业务逻辑
├── components/
│   ├── app_config/         # NVS 配置（node_id、传感器引脚配置、上报间隔）
│   ├── i2c_bus/            # I2C 主机总线（传感器与 OLED 共用，含整段访问互斥）
│   ├── sensor_driver/      # 传感器驱动层（按类型注册，统一采集接口）
│   ├── sensor_registry/    # 自身传感器类型注册表（握手时上报给 hub）
│   ├── ble_peripheral/     # BLE 外设广播、GATT 服务、握手响应
│   ├── data_pipeline/      # 定时采集 → 数据打包 → 交给 ble_peripheral 发送
│   ├── oled_display/       # SSD1315 0.96" OLED 实时显示（LVGL 9，可选外设）
│   └── power_manager/      # 低功耗管理（采集间隙 light sleep，可选）
├── CMakeLists.txt
└── sdkconfig.defaults
```

### 1. 组件职责切分

| 组件 | 职责 | 为什么单独成组件 |
| :--- | :--- | :--- |
| `app_config` | 跨重启配置（node_id、传感器类型/引脚映射、上报间隔、配对 hub_id） | NVS key 集中管理 |
| `i2c_bus` | I2C 主机总线：建总线、挂从设备、地址探测、整段访问 `Lock()/Unlock()` | 总线是共享资源，传感器与显示屏都要用；抽出后新增 I2C 设备不再重建总线 |
| `sensor_driver` | 各类传感器驱动（当前 SHT3X / BMP180），统一 `Read()` 接口 | 传感器可增删，独立后新增传感器只加驱动文件不动其他组件 |
| `sensor_registry` | 节点支持哪些传感器类型、各自数据格式描述 | 握手时需要上报能力清单，与驱动分离便于扩展 |
| `ble_peripheral` | BLE 广播、GATT 服务端、握手协议响应 | BLE 协议栈独立，握手逻辑可单独演进 |
| `data_pipeline` | 定时触发采集 → 调 sensor_driver → 打包 → 交给 ble 发送 | 采集节奏与传输分离，改上报策略只动本组件 |
| `oled_display` | SSD1315 OLED：页眉 node-id + 连接状态，主体 T/H/P 实时值 | 显示是可选外设，独立后未接屏只告警、不影响节点主流程 |
| `power_manager` | 采集间隙 light sleep、唤醒定时 | 电池供电场景可选启用，独立后不影响主流程 |

切分原则：**组件之间不互相创建或持有所有权，只通过 `Init()` 注入的指针 + 事件总线交互**。`ble_peripheral` 不需要知道传感器细节，握手时从 `sensor_registry` 取能力清单上报；`data_pipeline` 不需要知道 BLE 存在，采集完投递数据事件由 `ble_peripheral` 订阅发送；`oled_display` 只做只读查询（向 `sensor_registry` 取实时值、向 `ble_peripheral` 取连接状态），不改变他人状态。

`i2c_bus` 由 `main` 创建并持有，`sensor_driver` 与 `oled_display` 作为同级消费者从它拿到各自设备句柄——总线生命周期不再藏在某个驱动内部。

### 2. 每个组件在自己的 `Init()` 里 `xTaskCreate`

沿用 `esp32-broker`/`esp32-hub` 习惯：
- 采集定时、BLE 广播、握手响应都是长周期任务，各自在 `Init()` 内 `xTaskCreate` 启动。
- 没有集中式 `Run()`，任务即状态机。

### 3. 组件对象在 `app_main` 中声明为 `static`

`app_main` 返回后栈对象析构会导致后台任务回调访问悬空 `this`。改为 `static` 后对象常驻到系统重启。

### 4. 不使用 `xxxInterface` / 不使用 `Noxxx` 空对象

- 所有组件均设计为「始终提供服务」，不存在需要空实现兜底的场景。
- 传感器驱动也不用抽象基类 `ISensor`，而是用统一签名的函数指针表（C 风格 vtable），保持 ESP-IDF 习惯。

### 5. CMakeLists 显式列出源文件，不用 `file(GLOB)`

显式 `SRCS` 列表在增删文件时必须改 CMake，确保构建系统被重新触发。

## 组件依赖关系

```
        ┌──────────────┐
        │  app_config  │  配置存储（被各组件依赖）
        └──────┬───────┘
               │
   ┌───────────┼───────────────┬──────────────┐
   ▼           ▼               ▼              ▼
i2c_bus   sensor_driver   ble_peripheral  power_manager
   │           │               ▲
   │           │ 注册能力       │ 订阅数据事件
   │           ▼               │
   │     sensor_registry ──────┤
   │           ▲               │
   │  读实时值  │               │
   └──▶ oled_display ──────────┘（查询连接状态）

data_pipeline ──定时采集──▶ sensor_driver ──打包──▶ 数据事件 ──▶ ble_peripheral
```

- `i2c_bus` 由 `main` 创建；`sensor_driver`（SHT3X/BMP180）与 `oled_display`（SSD1315）通过它挂载各自从设备。
- `sensor_driver` 启动时向 `sensor_registry` 注册自身（类型 + 采集函数 + 数据格式）。
- `ble_peripheral` 握手时从 `sensor_registry` 取能力清单上报给 hub。
- `data_pipeline` 定时调 `sensor_registry.ReadAll()` 采集 → 投递数据事件 → `ble_peripheral` 订阅发送。
- `oled_display` 每 1s 调 `sensor_registry.ReadAll()` 取实时值，并向 `ble_peripheral` 查询配对/连接状态（未配对时屏上也要有实时值）。
- `power_manager` 在采集间隙与 BLE 空闲时进入 light sleep（可选）。

## 关键事件流

### 上电启动

```
上电
 ├─ app_config.Init()        加载 NVS（node_id/传感器配置/上报间隔）
 ├─ nimble_port_init()       BLE 协议栈 host
 ├─ sensor_registry.Init()   构建能力清单（类型列表 + 数据格式描述）
 ├─ i2c_bus.Init()           建 I2C 主机总线（400kHz，SDA/SCL 取自 app_config）
 ├─ sensor_driver.Init()     在总线上挂载各传感器，注册到 sensor_registry
 ├─ data_pipeline.Init()     创建采集定时任务（待配对完成后启动）
 ├─ ble_peripheral.Init()    配置广播数据、GATT 服务、启动广播
 ├─ oled_display.Init()      探测屏地址 → 建面板 → 挂 LVGL → 建界面 → 启动刷新任务
 └─ power_manager.Init()     注册空闲回调（可选）
```

### 蓝牙自动配对流程

```
ble_peripheral 广播（含 esp32-node 服务 UUID）
  └─ hub 主动 GATT 连接
      └─ 握手协议响应（与 esp32-hub 对应）
          ├─ 收到 hub 握手请求 { relay_id, version }
          ├─ 上报握手响应 { node_id, sensor_types[], version }
          ├─ 收到 hub 接受配对 { accepted: true, report_interval_ms }
          └─ 发送配对确认 { paired: true }
              └─ 保存 hub_id 到 NVS
              └─ 启动 data_pipeline 定时采集
```

### 传感器数据上报流程

```
data_pipeline 定时触发
  └─ sensor_driver.Read() 采集（遍历所有已注册传感器）
      └─ 数据打包（按传感器类型格式化）
          └─ 投递传感器数据事件
              └─ ble_peripheral 订阅 → GATT Notify 发送
                  └─ 等待下一周期
```

### 断线重连流程

```
GATT 连接断开
  └─ ble_peripheral 重新广播
      └─ 等待 hub 重新连接（hub 端也会自动重连已配对节点）
          └─ 重新握手（使用保存的 hub_id 快速配对）
              └─ data_pipeline 恢复采集
```

## 蓝牙握手协议（与 esp32-hub 对应）

```
1. hub → node：WRITE  请求     {"act":"hello","relay_id":"hub-1A2B","ver":1}
2. node → hub：NOTIFY 响应     {"act":"hello_ack","node_id":"node-1A2B","ver":1,
                                "types":["temp_hum","pressure"],"sensors":[...]}
3. hub → node：WRITE  接受配对 {"act":"accept","interval_ms":5000}
4. node → hub：NOTIFY 配对确认 {"act":"ack_done"}
```

握手成功后节点进入数据上报模式，按 `interval_ms`（保存到 `app_config`）间隔 NOTIFY 传感器数据。
GATT 定义：服务 `0000ff00-...`、WRITE 特征 `0000ff01-...`、NOTIFY 特征 `0000ff02-...`、
CCC 描述符 `00002902-...`（由协议栈自动挂在 `ff02 值句柄 + 1`，与 hub 端订阅方式一致）。

## 传感器注册机制

### 节点端注册（sensor_registry）

每个传感器驱动在 `Init()` 时向注册表登记：

```
type:           "temp_hum"               (类型标识)
model:          "SHT3X"                  (型号)
data_format:    { temp: "float", humidity: "float", unit: "C/%" }
read_func:      &Sht3x::ReadThunk        (采集函数指针，C 风格 vtable)
ctx:            &sht3x_                  (驱动实例)
```

> 驱动只填 `ts_ms` 与 `values_json`，`type` 由 `SensorRegistry` 在遍历采集时统一回填，
> 避免同一类型标识在多处重复维护。

### 握手时上报给 hub

```
能力清单 = [
  { type: "temp_hum", model: "SHT3X",  format: { temp, humidity, unit: "C/%" } },
  { type: "pressure", model: "BMP180", format: { pressure, temp, altitude, unit: "hPa/C/m" } }
]
```

hub 收到后写入 `node_registry` 并经 MQTT 上报 `hub/{relay_id}/node/{node_id}/register`，PC 端据此渲染传感器卡片。

### 数据上报格式

```
{
  "type": "temp_hum",
  "ts": 1234567890,
  "values": { "temp": 23.5, "humidity": 65.2 }
}
```

```
{
  "type": "pressure",
  "ts": 1234567890,
  "values": { "pressure": 1013.25, "temp": 22.1, "altitude": 112.3 }
}
```

每个传感器类型对应一个数据包，由 `data_pipeline` 按注册表格式打包，`sensor_pipeline`（hub 端）按 `type` 解析后上报 MQTT。
单位：温度 ℃、湿度 %RH、气压 hPa、海拔 m（海拔以 `app_config.sea_level_hpa` 为参考海平面气压换算）。

## 显示屏（oled_display）

0.96" SSD1315（128×64 单色，I2C 地址 `0x3C`，部分模块 `0x3D`，上电自动探测），
用 LVGL 9 + `espressif/esp_lvgl_port` 渲染，单页英文/数字布局（面板硬件旋转 180°，安装时排针侧为上）：

```
node-1A2B                ADV     ← 页眉 10px：左 node-id，右连接状态
─────────────────────────────    ← 1px 分隔线（y=13）
T                    23.5 °C     ← 标签 12px，数值 14px（行距 16px，y=15/31/47）
H                     65.2 %
P                  1013.2 hPa
```

- 方向：通过面板命令 `0xA1`（SEG 重映射）+ `0xC8`（COM 重映射）做硬件 180° 旋转，无软件旋转开销。
- 状态徽标：`ADV`（广播中）/ `CONN`（已连接未握手）/ `PAIRED`（配对完成）。
- 刷新：独立任务每 1s 采集一次；某行读失败显示 `-- °C` / `-- %` / `-- hPa`。
- 渲染：单色 `LV_COLOR_FORMAT_I1` 必须整屏缓冲，故采用整屏单缓冲；文本内容未变化时不重设 label，省掉一次整屏 I2C 传输。
- 取数：直接调 `sensor_registry.ReadAll()` 拿实时值，不订阅 `data_pipeline` 的采集事件——后者只在配对后采样，而屏上未配对时也要显示。
- 可选性：屏未接（`0x3C`/`0x3D` 均无应答）时 `OledDisplay::Init()` 返回 `ESP_ERR_NOT_FOUND`，`main` 仅打告警，节点照常广播与上报。

## 硬件

| 项目 | 说明 |
| :--- | :--- |
| 主控 | ESP32-C3 mini（4MB Flash） |
| 温湿度 | SHT3X / SHT30（I2C，7 位地址 `0x44`，ADDR 接高为 `0x45`） |
| 气压/温度/海拔 | BMP180 / GY-68（I2C，7 位地址 `0x77`） |
| 显示屏 | SSD1315 0.96" 128×64 单色 OLED（I2C，7 位地址 `0x3C`，部分模块 `0x3D`），可选 |
| 总线 | 上述设备共用同一条 I2C 总线，400kHz（模块板载上拉） |

I2C 引脚（可在 `app_config` 中持久化修改，默认值见 `AppConfig::kDefaultSda/kDefaultScl`）：

| 信号 | GPIO | 说明 |
| :--- | :--- | :--- |
| SDA | 4 | 各 I2C 设备 SDA 并联 |
| SCL | 5 | 各 I2C 设备 SCL 并联 |
| VCC | 3V3 | SHT3X 丝印 `VIN`，OLED 常见丝印 `VCC` |
| GND | GND | 共地 |

> 选 4/5 的原因：ESP32-C3 上避开 strapping（2/8/9）、片内 Flash（11~17）、
> USB（18/19）、UART0 控制台（20/21）。
>
> 总线跑 400kHz 的原因：100kHz 下一次整屏 OLED 刷新约 90ms，会明显拖住 LVGL 刷新任务，
> 400kHz 下约 23ms；SHT3X（≤1MHz）与 BMP180（≤3.4MHz）均支持 400kHz。

## 构建与烧录

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

> 沙箱内构建需加 `-DCCACHE_ENABLE=0`，本地 IDE 正常构建不受影响。
>
> `oled_display` 依赖 `lvgl/lvgl` 与 `espressif/esp_lvgl_port`（组件管理器的 managed component），
> 首次构建需联网从组件仓库拉取，之后走本地缓存。若不接显示屏，可从 `oled_display/idf_component.yml`
> 与该组件的 `CMakeLists.txt` 依赖中移除这两项。

## 使用流程

1. 按「硬件」小节接线（SHT3X、BMP180 与 OLED 共用 SDA=4 / SCL=5）。
   如需改引脚，改 `AppConfig` 默认值后重新烧录，或用 `AppConfig::SetI2cPins()` 写入 NVS。
2. 上电后自动初始化传感器并向 `sensor_registry` 注册能力清单；若接了 OLED，屏上开始显示实时值，
   页眉右侧状态徽标在 `ADV` / `CONN` / `PAIRED` 间切换。
3. BLE 开始广播，等待 `esp32-hub` 发现并连接。
4. hub 主动连接后完成握手，节点上报传感器类型，hub 注册到 MQTT。
5. 配对成功后按约定间隔采集并通过 GATT Notify 上报数据。
6. 断线后自动重新广播，等待 hub 重连。
7. 电池供电场景可在 `app_config` 中打开 `power_save`，启用 DFS + 自动 light sleep。
