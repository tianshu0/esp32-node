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
硬件相关部分再拆成三个**编译期可裁剪的正交维度**（对标 `esp32-xiaozhi` 的板型选择）：

- **板型**（`NODE_BOARD`）：引脚映射 + 装配关系，`main/boards/<板型>/` 下一个 Board.cpp 是全项目唯一知道具体硬件的文件；
- **传感器**（`NODE_SENSOR_*`，可多选）：`components/sensors/` 下每个驱动一个类，按 Kconfig 勾选编入；
- **显示屏**（`NODE_DISPLAY`，单选）：`components/display_service/` 下的显示驱动 + 内容模板，可选择「无屏」。

```
esp32-node/
├── main/
│   ├── main.cpp                 # 硬件无关的系统初始化（日志/NVS/BLE 栈）→ BoardAssemble()
│   ├── Kconfig.projbuild        # Node hardware profile：板型/传感器/显示屏三个选择维度
│   └── boards/                  # 板型装配层：引脚 + 具体实例化哪些设备
│       ├── Board.hpp            # NodeContext + BoardAssemble() 声明
│       └── c3_i2c_oled/         # 当前板型：C3 + I2C 传感器 + SSD1315 OLED
│           ├── Pins.hpp         # 本板引脚映射
│           └── Board.cpp        # 按 Kconfig 实例化 I2C 总线/传感器/屏幕
├── components/
│   ├── app_config/              # NVS 配置（node_id、上报间隔、海平面气压）
│   ├── i2c_bus/                 # I2C 主机总线（传感器与屏共用，含整段访问互斥）
│   ├── hardware_context/        # 硬件句柄集合（I2cBus 指针…），纯头文件，传给设备 Start()
│   ├── sensors/                 # 传感器插件层：SensorDevice 基类 + 各驱动（SHT3X/BMP180）
│   ├── sensor_registry/         # 传感器注册表：类型 + 字段描述符 + 统一采集函数
│   ├── display_service/         # 显示插件层：DisplayDevice/Screen 抽象 + SSD1315 驱动 + 仪表盘模板
│   ├── ble_peripheral/          # BLE 外设广播、GATT 服务、握手响应
│   ├── data_pipeline/           # 定时采集 → 数据打包 → 交给 ble_peripheral 发送
│   └── power_manager/           # 低功耗管理（采集间隙 light sleep，可选）
├── CMakeLists.txt
└── sdkconfig.defaults
```

### 1. 组件职责切分

| 组件 | 职责 | 为什么单独成组件 |
| :--- | :--- | :--- |
| `app_config` | 跨重启配置（node_id、上报间隔、海平面气压、配对 hub_id） | NVS key 集中管理 |
| `i2c_bus` | I2C 主机总线：建总线、挂从设备、地址探测、整段访问 `Lock()/Unlock()` | 总线是共享资源，传感器与显示屏都要用；抽出后新增 I2C 设备不重建总线 |
| `hardware_context` | `struct HardwareContext { I2cBus* i2c; }` 硬件句柄集合 | 纯头文件；传感器/显示驱动通过它拿总线，不直接依赖板型 |
| `sensors` | 传感器插件：抽象基类 `SensorDevice` + 各驱动（当前 SHT3X / BMP180），由 Kconfig 勾选编入 | 新增传感器只加一对 `XxxSensor.{hpp,cpp}` 和一个 Kconfig 开关，其他组件零改动 |
| `sensor_registry` | 已注册传感器的类型、**字段描述符**（key/标签/单位/小数位）、采集函数 | 握手时上报能力清单，也是数据驱动 UI 的行模型，与驱动分离 |
| `display_service` | 显示插件：驱动抽象 `DisplayDevice`（SSD1315）+ 内容抽象 `Screen`（`DashboardScreen` 仪表盘） | 「用什么屏」和「显示什么内容」两个变化方向分开；无屏板型选 `NODE_DISPLAY_NONE` |
| `ble_peripheral` | BLE 广播、GATT 服务端、握手协议响应 | BLE 协议栈独立，握手逻辑可单独演进 |
| `data_pipeline` | 定时触发采集 → 调 registry → 打包 → 交给 ble 发送 | 采集节奏与传输分离，改上报策略只动本组件 |
| `power_manager` | 采集间隙 light sleep、唤醒定时 | 电池供电场景可选启用，独立后不影响主流程 |

切分原则：**组件之间不互相创建或持有所有权，只通过 `Init()`/`Start()` 注入的引用 + 事件总线交互**。
`main.cpp` 本身与硬件完全无关——NVS/BLE 栈等公共初始化后调用 `BoardAssemble(ctx)`，
由 `main/boards/<板型>/Board.cpp` 这**唯一一个文件**决定建什么总线、实例化哪些传感器、装什么屏。
`ble_peripheral` 不需要知道传感器细节，握手时从 `sensor_registry` 取能力清单上报；
`data_pipeline` 不需要知道 BLE 存在，采集完投递数据事件由 `ble_peripheral` 订阅发送；
显示侧只做只读查询（向 `sensor_registry` 取实时值、向 `ble_peripheral` 取连接状态），不改变他人状态。

`i2c_bus` 由板型装配文件创建并放入 `HardwareContext`，各传感器驱动与显示驱动作为同级消费者从它拿到各自设备句柄——总线生命周期不藏在某个驱动内部。

### 2. 每个组件在自己的 `Init()` 里 `xTaskCreate`

沿用 `esp32-broker`/`esp32-hub` 习惯：
- 采集定时、BLE 广播、握手响应都是长周期任务，各自在 `Init()` 内 `xTaskCreate` 启动。
- 没有集中式 `Run()`，任务即状态机。

### 3. 组件对象在 `app_main` 中声明为 `static`

`app_main` 返回后栈对象析构会导致后台任务回调访问悬空 `this`。改为 `static` 后对象常驻到系统重启。

### 4. 插件用抽象基类多态，但不用 `xxxInterface` 命名、不用空对象

- 变化点用面向对象抽象基类表达：`SensorDevice`（传感器插件）、`DisplayDevice`（屏驱动）、`Screen`（内容模板），
  板型装配文件持有具体子类、以基类引用启动；类名按实现角色命名，不加 `Interface` 后缀。
- 核心/网络组件仍设计为「始终提供服务」，不引入 `NullXxx` 空实现；「无屏」由 Kconfig 在装配层整体跳过，而不是塞一个空屏对象。
- 传感器与注册表之间仍保留统一签名的采集函数指针（`ReadFn`），运行期遍历采集无需虚调用。

### 5. CMakeLists 显式列出源文件，不用 `file(GLOB)`

显式 `SRCS` 列表在增删文件时必须改 CMake，确保构建系统被重新触发。
注意 **REQUIRES 一律无条件声明**，只对 `SRCS` 做 `if(CONFIG_…)` 条件编译：ESP-IDF 在 Kconfig 变量
注入之前就有一轮组件依赖收集，条件化 REQUIRES 会在全新 configure 时丢依赖。

## 组件依赖关系

```
                    ┌──────────────┐
                    │  app_config  │  配置存储（被各组件依赖）
                    └──────┬───────┘
                           │
        ┌──────────────────┼────────────────────────┐
        ▼                  ▼                        ▼
     i2c_bus          ble_peripheral           power_manager
        │                  ▲
        │ 放进              │ 订阅数据事件
        ▼                  │
 hardware_context          │
   ┌────┴────┐             │
   ▼         ▼             │
sensors  display_service   │
   │  Start() 注册          │
   ▼                        │
sensor_registry ────────────┤
   ▲    ▲                   │
   │    │ 读实时值/状态       │
   │    └── display_service ┘（Screen 数据驱动，不认识具体传感器）
   │
data_pipeline ──定时 ReadAll──▶ 打包 ──▶ kEventSample ──▶ ble_peripheral

装配关系（编译期由 Kconfig 决定）：
main.cpp ──▶ BoardAssemble()  [main/boards/<NODE_BOARD>/Board.cpp]
                 ├─ new I2cBus + HardwareContext
                 ├─ #if NODE_SENSOR_*   → XxxSensor.Start(hw, config, registry)
                 └─ #if NODE_DISPLAY_*  → XxxDisplay.Start(DisplayContext{ screen, … })
```

- 板型装配文件创建 `i2c_bus`；勾选的传感器（SHT3X/BMP180）与选中的显示屏（SSD1315）通过 `HardwareContext` 拿到总线、挂各自从设备。
- 传感器驱动在 `Start()` 内向 `sensor_registry` 注册自身：类型 + 采集函数 + **字段描述符**（每个字段的 key/短标签/单位/小数位）。
- `ble_peripheral` 握手时从 `sensor_registry` 取能力清单上报给 hub。
- `data_pipeline` 定时调 `sensor_registry.ReadAll()` 采集 → 投递数据事件 → `ble_peripheral` 订阅发送。
- `DashboardScreen` 在 `Build()` 时遍历 `sensor_registry` 的字段描述符自动生成仪表盘行——加传感器后界面自动多一行，无需改 UI 代码。
- `display_service` 每 1s 调 `sensor_registry.ReadAll()` 取实时值，并向 `ble_peripheral` 查询配对/连接状态（未配对时屏上也要有实时值）。
- `power_manager` 在采集间隙与 BLE 空闲时进入 light sleep（可选）。

## 关键事件流

### 上电启动

```
上电（main.cpp，所有板型相同的固定链）
 ├─ app_config.Init()        加载 NVS（node_id/上报间隔/海平面气压）
 ├─ nimble_port_init()       BLE 协议栈 host
 ├─ sensor_registry.Init()   空注册表（具体条目由板型层登记）
 ├─ data_pipeline.Init()     创建采集定时任务（待配对完成后启动）
 ├─ ble_peripheral.Init()    配置广播数据、GATT 服务、启动广播
 ├─ BoardAssemble(ctx)       ★板型相关（main/boards/<板型>/Board.cpp）
 │    ├─ i2c_bus.Init()            建 I2C 主机总线（400kHz，SDA/SCL 取自 app_config）
 │    ├─ #if NODE_SENSOR_*         各 XxxSensor.Start()：挂从设备 + 注册到 registry
 │    └─ #if NODE_DISPLAY_*        XxxDisplay.Start()：探测屏 → 面板 → LVGL → Screen.Build()
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
  └─ sensor_registry.ReadAll() 采集（遍历所有已注册传感器）
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

每个传感器驱动在自身 `Start()` 时向注册表登记：

```
type:           "temp_hum"               (类型标识，SensorDevice::Type())
model:          "SHT3X"                  (型号)
format_json:    {"temp":"float", ...}    (线上数据格式，进入握手 JSON)
fields[]:       SensorField 描述符数组     (本地屏幕字段，不进 BLE)
read_func:      &XxxSensor::ReadThunk    (统一签名采集函数指针，C 风格 vtable)
ctx:            this                     (驱动实例)
```

`SensorField` 描述一个**可显示字段**：`key`（values_json 中的字段名）、`label`（屏幕短标签，如 `T`）、
`unit`（显示单位）、`decimals`（小数位）。显示模板 `DashboardScreen` 遍历这些描述符自动生成仪表盘行，
新增传感器无需改任何 UI 代码；不上屏的字段（如 BMP180 的 `altitude`）不登记 field，但仍在 `values_json` 中上报。

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

## 显示（display_service）

显示侧拆成两个抽象，回答两个不同的问题：

- **`DisplayDevice`——「用什么屏」**：面板/LVGL 初始化与刷新任务。当前实现 `Ssd1315Display`
  （0.96" SSD1315，128×64 单色，I2C 地址 `0x3C`/`0x3D` 上电自动探测）。
- **`Screen`——「显示什么内容」**：拿到 `lv_obj_t*` 根对象后构建 UI、接收读数刷新。
  当前实现 `DashboardScreen`：数据驱动仪表盘，行模型来自 `sensor_registry` 的字段描述符。

两者在板型装配文件里自由组合（例如将来可以 `Ssd1315Display` 配一个图表 Screen，
或同一块 `DashboardScreen` 换一块 SPI 屏驱动），互不认识。

默认仪表盘布局（面板硬件旋转 180°，安装时排针侧为上）：

```
node-1A2B                ADV     ← 页眉 10px：左 node-id，右连接状态
─────────────────────────────    ← 1px 分隔线（y=13）
T                    23.5 °C     ← 标签/数值行（行距 16px）
H                     65.2 %
P                  1013.2 hPa
```

- 行与字号完全由注册字段数决定：≤3 行用 14/12px，4 行用 12/10px，≥5 行用 10px（上限 6 行）。
- 方向：通过面板命令 `0xA1`（SEG 重映射）+ `0xC8`（COM 重映射）做硬件 180° 旋转，无软件旋转开销。
- 状态徽标：`ADV`（广播中）/ `CONN`（已连接未握手）/ `PAIRED`（配对完成）。
- 刷新：独立任务每 1s 采集一次；某行读失败显示 `--` 加原单位。
- 渲染：单色 `LV_COLOR_FORMAT_I1` 必须整屏缓冲，故采用整屏单缓冲；文本内容未变化时不重设 label，省掉一次整屏 I2C 传输。
- 取数：直接调 `sensor_registry.ReadAll()` 拿实时值，不订阅 `data_pipeline` 的采集事件——后者只在配对后采样，而屏上未配对时也要显示。
- 可选性：选 `CONFIG_NODE_DISPLAY_NONE` 时显示代码整体不编入；选 SSD1315 但屏未接，`Ssd1315Display::Start()` 返回错误，板型层仅打告警，节点照常广播与上报。

## 硬件扩展（menuconfig：Node hardware profile）

三个维度都在 `idf.py menuconfig` → **Node hardware profile** 里选（定义于 `main/Kconfig.projbuild`，编译期决定，零运行时开销）：

| 维度 | Kconfig | 选择方式 | 落地位置 |
| :--- | :--- | :--- | :--- |
| 板型（引脚 + 装配） | `CONFIG_NODE_BOARD_*` | 单选 choice | `main/boards/<板型>/` |
| 传感器 | `CONFIG_NODE_SENSOR_*` | 多选 bool | `components/sensors/` |
| 显示屏 | `CONFIG_NODE_DISPLAY_*` | 单选 choice（含 NONE） | `components/display_service/` |

### 加一个新传感器（以 I2C 传感器为例）

1. 在 `components/sensors/` 加 `XxxSensor.hpp/.cpp`，继承 `SensorDevice`：实现 `Type()`（类型标识）
   与 `Start(HardwareContext&, AppConfig&, SensorRegistry&)`——在里面 `i2c->AddDevice()`、初始化芯片、
   调 `registry.Register()` 登记类型/format_json/**字段描述符**/ReadThunk。
2. `components/sensors/CMakeLists.txt`：加 `if(CONFIG_NODE_SENSOR_XXX)` 把新 cpp 加入 `SRCS`（REQUIRES 不动）。
3. `main/Kconfig.projbuild` 的 Sensors 菜单加一个 `config NODE_SENSOR_XXX` bool 开关。
4. 板型 `Board.cpp` 加 `#if CONFIG_NODE_SENSOR_XXX` 分支：`static XxxSensor xxx; xxx.Start(hw, …)`。

完成后：握手清单、data_pipeline 采集、仪表盘行（自动多一行）全部自动生效，其他组件零改动。

### 加一种新显示屏

1. 在 `components/display_service/` 加 `XxxDisplay.hpp/.cpp`，继承 `DisplayDevice`：在 `Start(DisplayContext&)`
   里建面板、挂 LVGL，并把 UI 构建/刷新委托给 `ctx.screen`（复用 `DashboardScreen` 即可）。
2. CMakeLists 的 `SRCS` 加条件分支；`main/Kconfig.projbuild` 的 `NODE_DISPLAY` choice 加一个选项。
3. 板型 `Board.cpp` 加对应 `#if` 分支组合新驱动与 Screen。

### 加一个全新板型

1. 复制 `main/boards/c3_i2c_oled/` 为 `main/boards/<新板型>/`：改 `Pins.hpp`，按硬件改 `Board.cpp` 的装配。
2. `main/CMakeLists.txt` 加 `if(CONFIG_NODE_BOARD_<新板型>)` 把新 Board.cpp 加入 `SRCS`。
3. `main/Kconfig.projbuild` 的 `NODE_BOARD` choice 加选项。

> CMake 约定：只对 `SRCS` 条件化，**REQUIRES 保持无条件**（见上文「为什么是当前的项目结构」第 5 点）。

## 硬件

| 项目 | 说明 |
| :--- | :--- |
| 主控 | ESP32-C3 mini（4MB Flash） |
| 温湿度 | SHT3X / SHT30（I2C，7 位地址 `0x44`，ADDR 接高为 `0x45`） |
| 气压/温度/海拔 | BMP180 / GY-68（I2C，7 位地址 `0x77`） |
| 显示屏 | SSD1315 0.96" 128×64 单色 OLED（I2C，7 位地址 `0x3C`，部分模块 `0x3D`），可选 |
| 总线 | 上述设备共用同一条 I2C 总线，400kHz（模块板载上拉） |

I2C 引脚（可在 `app_config` 中持久化修改，默认值见 `AppConfig::kDefaultSda/kDefaultScl`，
板型默认映射见 `main/boards/c3_i2c_oled/Pins.hpp`）：

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
idf.py menuconfig    # 可选：Node hardware profile 里换板型/勾选传感器/选显示屏
idf.py build
idf.py -p COMx flash monitor
```

> 沙箱内构建需加 `-DCCACHE_ENABLE=0`，本地 IDE 正常构建不受影响。
>
> `display_service` 依赖 `lvgl/lvgl` 与 `espressif/esp_lvgl_port`（组件管理器的 managed component），
> 首次构建需联网从组件仓库拉取，之后走本地缓存。无屏板型选 `CONFIG_NODE_DISPLAY_NONE`
> 可让显示驱动源文件不编入（LVGL 仍参与构建，占用少量 flash）。

## 使用流程

1. 按「硬件」小节接线（SHT3X、BMP180 与 OLED 共用 SDA=4 / SCL=5）。
   如需改引脚，改 `AppConfig` 默认值后重新烧录，或用 `AppConfig::SetI2cPins()` 写入 NVS。
2. 上电后板型装配层初始化勾选的传感器并向 `sensor_registry` 登记；若配置了显示屏，屏上按登记字段自动生成仪表盘并显示实时值，
   页眉右侧状态徽标在 `ADV` / `CONN` / `PAIRED` 间切换。
3. BLE 开始广播，等待 `esp32-hub` 发现并连接。
4. hub 主动连接后完成握手，节点上报传感器类型，hub 注册到 MQTT。
5. 配对成功后按约定间隔采集并通过 GATT Notify 上报数据。
6. 断线后自动重新广播，等待 hub 重连。
7. 电池供电场景可在 `app_config` 中打开 `power_save`，启用 DFS + 自动 light sleep。
