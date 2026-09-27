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
硬件相关部分采用**「项目即宏」**的编译期裁剪机制（对标 `esp32-xiaozhi` 的板型选择；xiaozhi 的单位是 board，本仓库的单位是 project）：

- 在 `main/Kconfig.projbuild` 的 `NODE_PROJECT` choice 选**一个项目宏**，宏通过 Kconfig `select`
  自动锁定屏 / 传感器 / 触摸等内部开关，组件层对具体项目无感；
- 每个项目对应 `main/projects/<项目>/` 一个自包含目录：
  - `config.h`：板级编译期常量（引脚 / 通道，仅本项目 Board.cpp include）；
  - `config.json`：构建元数据（`target` 芯片 + `builds.sdkconfig_append`，供 `release.py` 使用）；
  - `Board.cpp`：全项目唯一知道具体硬件组合的文件（建什么总线、实例化哪些传感器、装什么屏）。

当前三个项目：

| 项目目录 | 芯片 | 显示屏 | 传感器 | 外设 |
| :--- | :--- | :--- | :--- | :--- |
| `c3_oled_thp` | ESP32-C3 | SSD1315 128x64 OLED | SHT3X 温湿度 + BMP180 气压 | NimBLE Peripheral |
| `c3_oled_voc` | ESP32-C3 | SSD1315 128x64 OLED | 21VOC（TVOC/甲醛/eCO2/温湿度，UART） | NimBLE Peripheral |
| `s3_tft_fan` | ESP32-S3 N16R8 | ILI9341 240x320 TFT + XPT2046 触摸 | AHT20 温湿度 + BMP280 气压（二合一模块） | PWM 风扇 + 自动化，WiFi 配网，无 BLE |

```
esp32-node/
├── main/
│   ├── main.cpp                 # 硬件无关的系统初始化（日志/NVS/BLE 栈）→ BoardAssemble()
│   ├── Board.hpp                # NodeContext + BoardAssemble() 声明（装配契约）
│   ├── Kconfig.projbuild        # NODE_PROJECT choice：选项目宏，select 锁定内部开关
│   └── projects/                # 项目装配层：每个目录一个自包含项目
│       ├── c3_oled_thp/         # C3 + OLED + 温湿度气压
│       │   ├── config.h         # 引脚等编译期常量
│       │   ├── config.json      # target + sdkconfig_append（release.py 读取）
│       │   └── Board.cpp        # 实例化 I2C 总线/传感器/屏幕
│       ├── c3_oled_voc/         # C3 + OLED + 21VOC（同上三件套）
│       └── s3_tft_fan/          # S3 + TFT 触摸 + SHT3X + PWM 风扇（同上三件套）
├── components/
│   ├── app_config/              # NVS 配置（node_id、上报间隔、海平面气压、WiFi）
│   ├── i2c_bus/                 # I2C 主机总线（传感器与屏共用，含整段访问互斥）
│   ├── hardware_context/        # 硬件句柄集合（I2cBus/UartBus 指针…），纯头文件，传给设备 Start()
│   ├── sensors/                 # 传感器插件层：SensorDevice 基类 + 各驱动（SHT3X/BMP180/VOC21）
│   ├── sensor_registry/         # 传感器注册表：类型 + 字段描述符 + 统一采集函数
│   ├── display_service/         # 显示插件层：DisplayDevice/Screen 抽象 + OLED/TFT 驱动 + 界面模板
│   ├── ble_peripheral/          # BLE 外设广播、GATT 服务、握手响应
│   ├── data_pipeline/           # 定时采集 → 数据打包 → 交给 ble_peripheral 发送
│   ├── fan_control/             # PWM 风扇控制（LEDC）
│   ├── automation/              # 自动化规则引擎（阈值 → 风扇动作）
│   ├── wifi_portal/             # SoftAP 网页配网
│   └── power_manager/           # 低功耗管理（采集间隙 light sleep，可选）
├── release.py                   # 一键构建：python release.py <项目> [--flash] [--monitor]
├── CMakeLists.txt
└── sdkconfig.defaults           # 仅放所有项目共享的最小默认项；项目差异在各自 config.json
```

### 1. 组件职责切分

| 组件 | 职责 | 为什么单独成组件 |
| :--- | :--- | :--- |
| `app_config` | 跨重启配置（node_id、上报间隔、海平面气压、配对 hub_id） | NVS key 集中管理 |
| `i2c_bus` | I2C 主机总线：建总线、挂从设备、地址探测、整段访问 `Lock()/Unlock()` | 总线是共享资源，传感器与显示屏都要用；抽出后新增 I2C 设备不重建总线 |
| `hardware_context` | `struct HardwareContext { I2cBus* i2c; }` 硬件句柄集合 | 纯头文件；传感器/显示驱动通过它拿总线，不直接依赖板型 |
| `sensors` | 传感器插件：抽象基类 `SensorDevice` + 各驱动（当前 SHT3X / BMP180 / VOC21），由项目宏 `select` 的内部开关条件编入 | 新增传感器只加一对 `XxxSensor.{hpp,cpp}` 和一个内部开关，其他组件零改动 |
| `sensor_registry` | 已注册传感器的类型、**字段描述符**（key/标签/单位/小数位）、采集函数 | 握手时上报能力清单，也是数据驱动 UI 的行模型，与驱动分离 |
| `display_service` | 显示插件：驱动抽象 `DisplayDevice`（SSD1315/ILI9341）+ 内容抽象 `Screen`（`DashboardScreen` 仪表盘 / `SpitftTouchDisplay` 触摸 UI） | 「用什么屏」和「显示什么内容」两个变化方向分开；项目未选中屏时对应源文件整体不编入 |
| `ble_peripheral` | BLE 广播、GATT 服务端、握手协议响应 | BLE 协议栈独立，握手逻辑可单独演进 |
| `data_pipeline` | 定时触发采集 → 调 registry → 打包 → 交给 ble 发送 | 采集节奏与传输分离，改上报策略只动本组件 |
| `power_manager` | 采集间隙 light sleep、唤醒定时 | 电池供电场景可选启用，独立后不影响主流程 |

切分原则：**组件之间不互相创建或持有所有权，只通过 `Init()`/`Start()` 注入的引用 + 事件总线交互**。
`main.cpp` 本身与硬件完全无关——NVS/BLE 栈等公共初始化后调用 `BoardAssemble(ctx)`，
由 `main/projects/<项目>/Board.cpp` 这**唯一一个文件**决定建什么总线、实例化哪些传感器、装什么屏。
`ble_peripheral` 不需要知道传感器细节，握手时从 `sensor_registry` 取能力清单上报；
`data_pipeline` 不需要知道 BLE 存在，采集完投递数据事件由 `ble_peripheral` 订阅发送；
显示侧只做只读查询（向 `sensor_registry` 取实时值、向 `ble_peripheral` 取连接状态），不改变他人状态。

`i2c_bus` 由项目装配文件创建并放入 `HardwareContext`，各传感器驱动与显示驱动作为同级消费者从它拿到各自设备句柄——总线生命周期不藏在某个驱动内部。

### 2. 每个组件在自己的 `Init()` 里 `xTaskCreate`

沿用 `esp32-broker`/`esp32-hub` 习惯：
- 采集定时、BLE 广播、握手响应都是长周期任务，各自在 `Init()` 内 `xTaskCreate` 启动。
- 没有集中式 `Run()`，任务即状态机。

### 3. 组件对象在 `app_main` 中声明为 `static`

`app_main` 返回后栈对象析构会导致后台任务回调访问悬空 `this`。改为 `static` 后对象常驻到系统重启。

### 4. 插件用抽象基类多态，但不用 `xxxInterface` 命名、不用空对象

- 变化点用面向对象抽象基类表达：`SensorDevice`（传感器插件）、`DisplayDevice`（屏驱动）、`Screen`（内容模板），
  板型装配文件持有具体子类、以基类引用启动；类名按实现角色命名，不加 `Interface` 后缀。
- 核心/网络组件仍设计为「始终提供服务」，不引入 `NullXxx` 空实现；某个项目不需要屏/风扇时，对应内容在该项目的 `Board.cpp` 中整体不出现，而不是塞一个空对象。
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

装配关系（编译期由 NODE_PROJECT 项目宏决定）：
main.cpp ──▶ BoardAssemble()  [main/projects/<项目>/Board.cpp]
                 ├─ new I2cBus / UartBus / SPI 总线 + HardwareContext
                 ├─ 该项目选定的传感器 XxxSensor.Start(hw, config, registry)
                 └─ 该项目选定的屏   XxxDisplay.Start(DisplayContext{ screen, … })
```

- 项目装配文件创建总线；该项目选定的传感器与显示屏通过 `HardwareContext` 拿到总线、挂各自从设备。
- 传感器驱动在 `Start()` 内向 `sensor_registry` 注册自身：类型 + 采集函数 + **字段描述符**（每个字段的 key/短标签/单位/小数位）。
- `ble_peripheral` 握手时从 `sensor_registry` 取能力清单上报给 hub。
- `data_pipeline` 定时调 `sensor_registry.ReadAll()` 采集 → 投递数据事件 → `ble_peripheral` 订阅发送。
- `DashboardScreen` 在 `Build()` 时遍历 `sensor_registry` 的字段描述符自动生成仪表盘行——加传感器后界面自动多一行，无需改 UI 代码。
- `display_service` 每 1s 调 `sensor_registry.ReadAll()` 取实时值，并向 `ble_peripheral` 查询配对/连接状态（未配对时屏上也要有实时值）。
- `power_manager` 在采集间隙与 BLE 空闲时进入 light sleep（可选）。

## 关键事件流

### 上电启动

```
上电（main.cpp，所有项目相同的固定链；BLE 段按 CONFIG_BT_ENABLED 条件编译，仅 C3 项目编入）
 ├─ app_config.Init()        加载 NVS（node_id/上报间隔/海平面气压）
 ├─ nimble_port_init()       BLE 协议栈 host（仅 C3 BLE 项目）
 ├─ sensor_registry.Init()   空注册表（具体条目由项目装配层登记）
 ├─ data_pipeline.Init()     创建采集定时任务（待配对完成后启动）
 ├─ ble_peripheral.Init()    配置广播数据、GATT 服务、启动广播（仅 C3 BLE 项目）
 ├─ BoardAssemble(ctx)       ★项目相关（main/projects/<项目>/Board.cpp）
 │    ├─ 总线 Init()               I2C/UART/SPI（SDA/SCL 默认取自 app_config）
 │    ├─ 选定传感器各 XxxSensor.Start()：挂从设备 + 注册到 registry
 │    └─ 选定屏 XxxDisplay.Start()：探测屏 → 面板 → LVGL → Screen.Build()
 ├─ power_manager.Init()     注册空闲回调（可选）
 └─ wifi_portal.Init()       SoftAP 网页配网（s3_tft_fan 经此配 WiFi）
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

- **`DisplayDevice`——「用什么屏」**：面板/LVGL 初始化与刷新任务。当前实现：
  `Ssd1315Display`（0.96" SSD1315，128×64 单色，I2C 地址 `0x3C`/`0x3D` 上电自动探测，两个 C3 项目使用）
  与 `SpitftTouchDisplay`（ILI9341 240×320 SPI TFT + XPT2046 触摸，s3_tft_fan 项目使用）。
- **`Screen`——「显示什么内容」**：拿到 `lv_obj_t*` 根对象后构建 UI、接收读数刷新。
  当前实现：`DashboardScreen`（OLED 数据驱动仪表盘，行模型来自 `sensor_registry` 的字段描述符）
  与 `SpitftTouchDisplay` 内的多页触摸 UI（风扇开关 / 状态 / 湿度）。

两者在项目装配文件里自由组合（例如将来可以 `Ssd1315Display` 配一个图表 Screen，
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
- 可选性：项目未选中的显示屏，其驱动源文件整体不编入；选中 SSD1315 但屏未接时 `Ssd1315Display::Start()` 返回错误，项目装配层仅打告警，节点照常广播与上报。

## 项目配置机制与硬件扩展

「项目即宏」在 `idf.py menuconfig` → **Node project → Project profile** 里选一个项目
（定义于 `main/Kconfig.projbuild`，编译期决定，零运行时开销）：

| 层级 | Kconfig 符号 | 性质 | 消费方 |
| :--- | :--- | :--- | :--- |
| 项目（用户唯一入口，单选 choice） | `CONFIG_NODE_PROJECT_*` | 三个项目之一，默认 `C3_OLED_THP` | `main/CMakeLists.txt` 选择项目目录 |
| 内部开关（无 prompt，被项目 `select` 锁定） | `CONFIG_NODE_SENSOR_*` | 传感器条件编入 | `components/sensors/CMakeLists.txt` |
| 内部开关（同上） | `CONFIG_NODE_DISPLAY_*` / `CONFIG_NODE_TOUCH_XPT2046` | 屏/触摸条件编入 | `components/display_service/CMakeLists.txt` 与源码 `#if` |

项目间除 Kconfig 外的另一部分差异（目标芯片、BT/PSRAM/LVGL/分区表等 sdkconfig 项）
放在各项目 `config.json` 的 `builds.sdkconfig_append` 中，由 `release.py` 构建时生成
`build/node-build.sdkconfig.defaults` 片段拼接到根 `sdkconfig.defaults` 之后。

### 加一个新传感器（以 I2C 传感器为例）

1. 在 `components/sensors/` 加 `XxxSensor.hpp/.cpp`，继承 `SensorDevice`：实现 `Type()`（类型标识）
   与 `Start(HardwareContext&, AppConfig&, SensorRegistry&)`——在里面 `i2c->AddDevice()`、初始化芯片、
   调 `registry.Register()` 登记类型/format_json/**字段描述符**/ReadThunk。
2. `components/sensors/CMakeLists.txt`：加 `if(CONFIG_NODE_SENSOR_XXX)` 把新 cpp 加入 `SRCS`（REQUIRES 不动）。
3. `main/Kconfig.projbuild` 的内部开关区加一个无 prompt 的 `config NODE_SENSOR_XXX bool`。
4. 在使用该传感器的项目 `Board.cpp` 里实例化：`static XxxSensor xxx; xxx.Start(hw, …)`，
   并在该项目的 `config NODE_PROJECT_*` 下加一行 `select NODE_SENSOR_XXX`。

完成后：握手清单、data_pipeline 采集、仪表盘行（自动多一行）全部自动生效，其他组件零改动。

### 加一种新显示屏

1. 在 `components/display_service/` 加 `XxxDisplay.hpp/.cpp`，继承 `DisplayDevice`：在 `Start(DisplayContext&)`
   里建面板、挂 LVGL，并把 UI 构建/刷新委托给 `ctx.screen`（复用 `DashboardScreen` 即可）。
2. CMakeLists 的 `SRCS` 加条件分支；`main/Kconfig.projbuild` 内部开关区加 `config NODE_DISPLAY_XXX bool`。
3. 使用新屏的项目 `Board.cpp` 组合新驱动与 Screen，并在其项目宏下 `select NODE_DISPLAY_XXX`。

### 加一个全新项目

1. 新建 `main/projects/<新项目>/`：
   - `config.h`：引脚/通道等编译期常量；
   - `Board.cpp`：装配实现（建总线、实例化传感器与屏，可从相近项目复制裁剪）；
   - `config.json`：`target`（esp32c3/esp32s3）+ `builds[0].sdkconfig_append`
     （首行必须是 `CONFIG_NODE_PROJECT_<宏名>=y`，其余为该项目的 sdkconfig 项）。
2. `main/Kconfig.projbuild` 的 `NODE_PROJECT` choice 加一个 `config NODE_PROJECT_<NAME>`，
   用 `select` 锁定所需内部开关。
3. `main/CMakeLists.txt` 加一个 `elseif(CONFIG_NODE_PROJECT_<NAME>) set(PROJECT_DIR "projects/<目录>")`。
4. `python release.py <新项目>` 构建验证。

> CMake 约定：只对 `SRCS` 条件化，**REQUIRES 保持无条件**（见上文「为什么是当前的项目结构」第 5 点）。

## 硬件

### C3 项目（c3_oled_thp / c3_oled_voc）

| 部件 | 说明 |
| :--- | :--- |
| 主控 | ESP32-C3 mini（4MB Flash） |
| 温湿度 | SHT3X / SHT30（I2C，7 位地址 `0x44`，ADDR 接高为 `0x45`），仅 c3_oled_thp |
| 气压/温度/海拔 | BMP180 / GY-68（I2C，7 位地址 `0x77`），仅 c3_oled_thp |
| 空气质量 | 21VOC 五合一（UART 9600，TVOC/CH2O 甲醛/eCO2/温湿度），仅 c3_oled_voc |
| 显示屏 | SSD1315 0.96" 128×64 单色 OLED（I2C，7 位地址 `0x3C`，部分模块 `0x3D`） |
| 总线 | I2C 400kHz（传感器与 OLED 共用，模块板载上拉）；21VOC 独占 UART1 TX=6/RX=7 |

I2C 引脚（可在 `app_config` 中持久化修改，默认值见 `AppConfig::kDefaultSda/kDefaultScl`，
项目默认映射见 `main/projects/c3_oled_*/config.h`）：

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

### S3 项目（s3_tft_fan）

| 部件 | 说明 |
| :--- | :--- |
| 主控 | ESP32-S3-WROOM-1-N16R8（16MB OCTAL Flash + 8MB OCTAL PSRAM） |
| 温湿度/气压 | AHT20 + BMP280 二合一模块（共用 I2C，SDA=8 / SCL=9；AHT20 `0x38`，BMP280 `0x76`） |
| 显示屏 | ILI9341 2.4" 240×320 RGB SPI TFT（SPI2_HOST：MOSI=11/MISO=10/SCK=12/CS=13/DC=14/RST=21/BL=47） |
| 触摸 | XPT2046（独立 SPI3_HOST，无 DMA：SCK=15/MOSI=16/MISO=17/CS=48，轮询模式） |
| 风扇 | 4 线 PWM 风扇，LEDC 25kHz/8bit，GPIO1 |

完整引脚见 `main/projects/s3_tft_fan/config.h`。

## 构建与烧录

在**已初始化的 ESP-IDF shell** 中（Windows 用 ESP-IDF PowerShell/CMD 快捷方式，
Linux 先 `. $IDF_PATH/export.sh`），于仓库根目录执行：

```bash
python release.py --list                    # 列出全部项目
python release.py c3_oled_thp               # 配置 + 编译
python release.py s3_tft_fan --flash -p COM7         # 编译 + 烧录
python release.py c3_oled_voc --flash --monitor -p COM7  # 烧录 + 串口监视
```

`release.py` 会读取所选项目 `config.json` 的 `target` 与 `sdkconfig_append`，
自动生成 sdkconfig 片段并完成 `reconfigure`；跨芯片切换（如 S3 构建后切 C3）时
自动清空 build/ 与 sdkconfig，无需手动 fullclean/set-target。

手动方式（不经过 release.py）：

```bash
idf.py set-target esp32c3      # 或 esp32s3
idf.py menuconfig              # Node project -> Project profile -> 选项目
idf.py build
idf.py -p COMx flash monitor
```

> 注意：手动方式下项目专属 sdkconfig 项（BT/NimBLE、PSRAM、LVGL 池/字号、分区表等）
> 不会自动应用，需参照对应 `main/projects/<项目>/config.json` 的 `sdkconfig_append` 自行勾选。
>
> `display_service` 依赖 `lvgl/lvgl` 与 `espressif/esp_lvgl_port`（组件管理器的 managed component），
> 首次构建需联网从组件仓库拉取，之后走本地缓存。

## 使用流程（C3 BLE 节点）

1. 按「硬件」小节接线（c3_oled_thp：SHT3X、BMP180 与 OLED 共用 SDA=4 / SCL=5；
   c3_oled_voc：OLED 接 I2C、21VOC 接 UART1 TX=6/RX=7）。
   如需改 I2C 引脚，改 `AppConfig` 默认值后重新烧录，或用 `AppConfig::SetI2cPins()` 写入 NVS。
2. 上电后项目装配层初始化选定的传感器并向 `sensor_registry` 登记；若配置了显示屏，屏上按登记字段自动生成仪表盘并显示实时值，
   页眉右侧状态徽标在 `ADV` / `CONN` / `PAIRED` 间切换。
3. BLE 开始广播，等待 `esp32-hub` 发现并连接。
4. hub 主动连接后完成握手，节点上报传感器类型，hub 注册到 MQTT。
5. 配对成功后按约定间隔采集并通过 GATT Notify 上报数据。
6. 断线后自动重新广播，等待 hub 重连。
7. 电池供电场景可在 `app_config` 中打开 `power_save`，启用 DFS + 自动 light sleep。

## 使用流程（S3 风扇控制屏）

1. 烧录 s3_tft_fan 后，手机连接 `FanNode-XXXX` 热点，在自动弹出的网页中配置 WiFi；
   也可在网页上手动开关风扇。
2. 上电后 TFT 显示状态页（时间/日期、温湿度、风扇状态、大开关），触摸操作风扇。
3. 自动化规则按温湿度阈值自动控制风扇（迟滞 + 定时时长保护）。
