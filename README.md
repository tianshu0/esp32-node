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
  自动锁定屏 / 传感器 / 触摸等内部开关，通用层对具体项目无感；
- 每个项目对应 `main/boards/<项目>/` 一个自包含目录：
  - `config.h`：板级编译期常量（引脚 / 通道，仅本项目 `<Xxx>Board.cpp` include）；
  - `config.json`：构建元数据（`target` 芯片 + `builds.sdkconfig_append`，供 `release.py` 使用）；
  - `<Xxx>Board.cpp`：全项目唯一知道具体硬件组合的文件（建什么总线、实例化哪些传感器、装什么屏、用哪种链路）。

当前三个项目：

| 项目目录 | 芯片 | 显示屏 | 传感器 | 外设 |
| :--- | :--- | :--- | :--- | :--- |
| `c3_oled_thp` | ESP32-C3 | SSD1315 128x64 OLED | SHT3X 温湿度 + BMP180 气压 | NimBLE Peripheral |
| `c3_oled_voc` | ESP32-C3 | SSD1315 128x64 OLED | 21VOC（TVOC/甲醛/eCO2/温湿度，UART） | NimBLE Peripheral |
| `s3_tft_fan` | ESP32-S3 N16R8 | ILI9341 240x320 TFT + XPT2046 触摸 | AHT20 温湿度 + BMP280 气压（二合一模块） | PWM 风扇 + 自动化，WiFi 配网，无 BLE |

```
esp32-node/
├── main/
│   ├── main.cpp                 # app_main：构造 Application → Init() → Run()
│   ├── application.hpp/.cpp     # 硬件无关的系统初始化 + 板级装配 + 1s 采集任务（无传输分支）
│   ├── Kconfig.projbuild        # NODE_PROJECT choice：选项目宏，select 锁定内部开关
│   ├── CMakeLists.txt           # 显式 SRCS 列表 + 项目宏 → boards/<name> 目录映射
│   ├── boards/                  # 板装配层：每个目录一块自包含的板
│   │   ├── board.hpp            #   NodeContext + Board 基类（总线原语 / GetDisplay() / GetLink()，装配契约）
│   │   ├── c3_oled_thp/         #   C3 + OLED + 温湿度气压
│   │   │   ├── config.h         #     引脚等编译期常量
│   │   │   ├── config.json      #     target + sdkconfig_append（release.py 读取）
│   │   │   ├── c3_oled_thp_board.hpp/.cpp   # 唯一知道本板硬件组合的文件
│   │   │   └── thp_display.hpp/.cpp         # 本板屏驱动（SSD1315 + 仪表盘）
│   │   ├── c3_oled_voc/         #   C3 + OLED + 21VOC（UART）
│   │   └── s3_tft_fan/          #   S3 + TFT 触摸 + AHT20/BMP280 + PWM 风扇（WiFi 链路）
│   ├── common/                  # 跨项目通用模块（一个职责一个目录）
│   │   ├── app_config/          #   NVS 配置（node_id、上报间隔、海平面气压、WiFi）
│   │   ├── sensor_registry/     #   传感器注册表：类型 + 字段描述符 + 统一采集函数
│   │   ├── ble/                 #   BLE 外设：GATT 服务、握手响应、周期采集上报
│   │   ├── wifi/                #   SoftAP 网页配网 + STA（web/index.html 编译期嵌入）
│   │   ├── link/                #   ★无线链路抽象：link.hpp + ble_link / wifi_link
│   │   ├── fan_control/         #   PWM 风扇控制（LEDC）
│   │   ├── automation/          #   自动化规则引擎（阈值 → 风扇动作）
│   │   └── power_manager/       #   低功耗管理（采集间隙 light sleep，可选）
│   ├── sensors/                 # 传感器插件层：SensorDevice 基类 + 各驱动（SHT3X/BMP180/AHT20/BMP280/VOC21）
│   └── display/                 # 显示基类与兜底（display.hpp / no_display.hpp + 中文字体）
├── partitions/                  # 分区表：partitions.csv（C3）/ partitions_s3.csv（S3）
├── scripts/
│   └── release.py               # 一键构建：python scripts/release.py <项目> [--flash] [--monitor]
├── CMakeLists.txt
└── sdkconfig.defaults           # 仅放所有项目共享的最小默认项；项目差异在各自 config.json
```

### 1. 模块职责切分

| 模块 | 职责 | 为什么单独成模块 |
| :--- | :--- | :--- |
| `app_config` | 跨重启配置（node_id、上报间隔、海平面气压、配对 hub_id、WiFi 凭据） | NVS key 集中管理 |
| `boards/board.hpp` 总线原语 | 板级总线接口：`Board` 暴露地址式 `I2cWrite/I2cRead/I2cWriteRead/I2cProbe` 与 `UartRead/UartWrite`，不出现 ESP-IDF 句柄；具体读写由各板在自己的 `<Xxx>Board.cpp` 用 ESP-IDF 原语实现，只覆盖用到的原语 | 总线是共享资源、但实现方式因板而异；抽象收进装配契约，避免为 I2C/UART 各建一个模块+句柄传递链 |
| `sensors` | 传感器插件：抽象基类 `SensorDevice` + 各驱动（SHT3X / BMP180 / AHT20 / BMP280 / VOC21），由项目宏 `select` 的内部开关条件编入 | 新增传感器只加一对 `XxxSensor.{hpp,cpp}` 和一个内部开关，其他模块零改动 |
| `sensor_registry` | 已注册传感器的类型、**字段描述符**（key/标签/单位/小数位）、采集函数 | 握手时上报能力清单，也是数据驱动 UI 的行模型，与驱动分离 |
| `display` + `boards/<Xxx>Display` | 驱动抽象 `Display`（SSD1315 单色 / ILI9341 + XPT2046 触摸）与内容（仪表盘 / 触摸 UI）；具体屏驱动随板放在 `boards/<name>/` | 「用什么屏」和「显示什么内容」两个变化方向分开；未选中屏的项目其驱动整体不编入 |
| `ble` | BLE 广播、GATT 服务端、握手协议响应、配对后周期采集与 Notify 上报 | BLE 协议栈独立；采集节奏（interval_ms 由 hub 下发、配对开始/断开停止）本质是 BLE 会话参数，内聚一处 |
| `wifi` | SoftAP 网页配网 + STA 上报（网页 `web/index.html` 编译期嵌入固件） | 配网是 S3 板独有外设，与 BLE 链路并列且互不感知 |
| `link` | **无线链路抽象**：`Link` 基类 + `BleLink` / `WifiLink` 实现，把「节点用什么方式通信」收敛为一个可替换对象 | 板装配层二选一（组合而非编译期 `#if`）；将来一块板要 BLE+WiFi 组合只需新增一个 Link 实现，`Application` 与其它板零改动 |
| `fan_control` / `automation` | PWM 风扇控制（LEDC）与阈值规则引擎（迟滞 + 定时保护） | 仅 S3 板使用，由板装配层可选装配 |
| `power_manager` | 采集间隙 light sleep、唤醒定时 | 电池供电场景可选启用，独立后不影响主流程 |

切分原则：**模块之间不互相创建或持有所有权，只通过 `Init()`/`Start()` 注入的引用交互**。
`application.cpp` 本身与硬件完全无关——事件循环 / NVS / 注册表等公共初始化后调用 `Board::Assemble()`，
再由 `board_->GetLink()->Start()` 启动链路；具体「建什么总线、实例化哪些传感器、装什么屏、用 BLE 还是 WiFi」
全部由 Kconfig 选中的 `main/boards/<项目>/<Xxx>Board.cpp` 这**唯一一类文件**决定。
传感器驱动在 `Start()` 时自登记到 `sensor_registry`；`ble` / `wifi` 从注册表取能力清单上报与采集，彼此不知道对方存在；
显示侧只做只读查询（向 `sensor_registry` 取实时值、向当前 `Link` 取连接状态文本），不改变他人状态。

### 1b. 链路抽象（link）：为什么不用编译期 `#if` 选传输

节点的传输身份（BLE 节点 / WiFi 节点）是**板的属性**，不是「共享逻辑里的一个分支」。若在 `Application` 里用
`#if CONFIG_BT_ENABLED` 决定初始化 BLE 还是 WiFi，传输身份就散落在 Kconfig + Application 两处，
将来出现「BLE + WiFi 双链路」的板型时 `#if` 组合会迅速膨胀。

改为：`Board` 在 `Assemble()` 内创建自己的 `Link` 子类并存入 `Board::link_`，
`Application` 只面向 `Link` 接口调用：

```cpp
Link* link = board_->GetLink();
ESP_RETURN_ON_ERROR(link->Start(), TAG, "link start failed");   // 启动
disp->SetStatus(board_->GetLink()->StatusText());               // 状态文本
```

- `BleLink` 内部先 `nimble_port_init()` 再注册 GATT 服务，`StatusText()` 返回 `ADV` / `CONN` / `PAIRED`；
- `WifiLink` 内部驱动 `wifi`（SoftAP 配网 + STA），`StatusText()` 返回 `已连接` / `未连接`；
- 二者都以**组合**方式持有协议模块，不继承 `Wifi` / `Ble`，因此不存在多继承；
- 用组合而非多继承的另一个收益：C3 板与 S3 板共用同一个 `Board` 基类，板之间不共享传输实现。

总线由板装配文件自己创建并持有（如 `i2c_bus_` 私有成员），对传感器驱动只暴露 `Board` 上的地址式原语
（`I2cWrite/I2cWriteRead/...`）——驱动不持有任何 ESP-IDF 句柄，总线生命周期不藏在某个驱动内部。

### 2. 长周期任务由所属模块在自己的 `Init()`/`Start()` 里 `xTaskCreate`

沿用 `esp32-broker`/`esp32-hub` 习惯：
- 采集定时、BLE 广播、握手响应都是长周期任务，各自在所属模块的 `Init()`/`Start()` 内 `xTaskCreate` 启动。
- `Application` 只保留一条与硬件无关的 1s 采集任务（读传感器 → 刷屏），没有集中式 `Run()`，任务即状态机。
- `xTaskCreate` 返回值必须校验，失败即上报错误，避免任务静默不启动。

### 3. 长生命周期对象一律 `static`

`app_main` 返回后栈对象析构会导致后台任务回调访问悬空 `this`。板装配层创建的总线/传感器/屏/链路
以及 `main.cpp` 内的 `Application` 都声明为 `static`，常驻到系统重启。

### 4. 插件用抽象基类多态，但不用 `xxxInterface` 命名、不用空对象

- 变化点用面向对象抽象基类表达：`SensorDevice`（传感器插件）、`Display`（屏驱动/内容）、`Link`（无线链路），
  板装配文件持有具体子类、以基类指针交给 `Board`；类名按实现角色命名，不加 `Interface` 后缀。
- 核心/网络模块（`ble` / `wifi` / `sensor_registry`）设计为「始终提供服务」，不引入 `NullXxx` 空实现；
  板装配层用不到的外设（如风扇）就整体不出现，而不是塞一个空对象。
- 屏是**可选外设**：探测/初始化失败时由板装配层切换为 `NoDisplay` 兜底，保证 `Board::GetDisplay()`
  永不返回 `nullptr`，节点照常上报。
- 传感器与注册表之间仍保留统一签名的采集函数指针（`ReadFn`），运行期遍历采集无需虚调用。
- `wifi` 对风扇的依赖用 `FanHooks`（`std::function` 回调组）反转：由 S3 板在装配时把 `FanControl`
  的方法绑成回调注入，`wifi` 只判断 `fan.valid()`，`include` 里不再出现 `fan_control`。

### 5. CMakeLists 显式列出源文件，不用 `file(GLOB)`

显式 `SRCS` 列表在增删文件时必须改 CMake，确保构建系统被重新触发。
注意 **REQUIRES 一律无条件声明**，只对 `SRCS` 做 `if(CONFIG_…)` 条件编译：ESP-IDF 在 Kconfig 变量
注入之前就有一轮组件依赖收集，条件化 REQUIRES 会在全新 configure 时丢依赖。

## 模块依赖关系

```
        ┌──────────────┐
        │  app_config  │  NVS 配置（被各模块依赖）
        └──────┬───────┘
               │
  ┌────────────┴───────────────────┬──────────────────┐
  ▼                                ▼                  ▼
power_manager              sensor_registry            Link
                                   ▲                   │
                                   │ Start() 自登记     │ 由板装配层二选一并组合
                                   │                   ▼
            Board（boards/board.hpp）──地址式总线原语──▶ sensors
              ▲                    └─地址式/裸句柄────▶ 显示（boards/<Xxx>Display）──读实时值──┘
              │ Assemble() 建总线（I2C/UART/SPI）并持有
        Application::Init()

装配关系（编译期由 NODE_PROJECT 项目宏决定）：

Application::Init() ──▶ GetBoard(ctx).Assemble()   [main/boards/<项目>/<Xxx>Board.cpp]
                          ├─ 建总线 I2C / UART / SPI（板私有成员，如 i2c_bus_）
                          ├─ 该项目选定的传感器 XxxSensor.Start(*this, config, registry)
                          ├─ 该项目选定的屏   → Board::display_
                          └─ 该项目选定的链路 new BleLink(...) / new WifiLink(...) → Board::link_

Application ──▶ board_->GetLink()->Start()          启动链路（不感知 BLE/WiFi）
Application ──▶ board_->GetLink()->StatusText()     1s 刷新屏上状态徽标
Application ──▶ registry_.ReadAll()                 1s 读实时值推给屏

BleLink ──持有──▶ ble ──配对后定时 ReadAll──▶ 打包 ──▶ tx 队列 ──▶ GATT Notify
WifiLink ──持有──▶ wifi（SoftAP 配网 + STA）◀──FanHooks 回调── 板装配层注入
```

- 板装配文件创建总线并持有（私有句柄），对外只把 `Board` 的地址式原语（`I2cWrite/I2cWriteRead/UartRead/...`）交给传感器驱动。
- 该项目选定的传感器 `Start(*this, ...)` 后经这些原语访问总线；屏因 `esp_lcd_new_panel_io_i2c` 需要裸总线句柄，由板直接传入 `i2c_bus_`。
- 传感器驱动在 `Start()` 内向 `sensor_registry` 注册自身：类型 + 采集函数 + **字段描述符**（每个字段的 key/短标签/单位/小数位）。
- 板装配文件创建自己的 `Link` 子类交给 `Board::link_`；`Application` 只调 `Link::Start()` 与 `Link::StatusText()`，不感知传输类型。
- `BleLink` 持有的 `ble` 握手时从 `sensor_registry` 取能力清单上报给 hub。
- `BleLink` 配对成功后按 `interval_ms` 定时调 `sensor_registry.ReadAll()` 采集 → 打包入 tx 队列 → 发送任务 GATT Notify 上报。
- `WifiLink` 持有的 `wifi` 负责 SoftAP 配网与 STA；风扇控制以 `FanHooks` 回调注入，`wifi` 不依赖 `fan_control`。
- 显示内容在 `Build()` 时遍历 `sensor_registry` 的字段描述符自动生成仪表盘行——加传感器后界面自动多一行，无需改 UI 代码。
- 显示侧每 1s 调 `sensor_registry.ReadAll()` 取实时值，并向 `board_->GetLink()` 查询状态文本（未配对时屏上也要有实时值）。
- `power_manager` 在采集间隙与链路空闲时进入 light sleep（可选）。

## 关键事件流

### 上电启动

```
上电（main.cpp → Application::Init()，所有项目相同的固定链，无传输分支）
 ├─ esp_event_loop_create_default()  默认事件循环（WiFi 需要）
 ├─ app_config.Init()                加载 NVS（node_id/上报间隔/海平面气压/WiFi 凭据）
 ├─ sensor_registry.Init()           空注册表（具体条目由板装配层登记）
 ├─ GetBoard(ctx).Assemble()         ★项目相关（main/boards/<项目>/<Xxx>Board.cpp）
 │    ├─ 建总线并持有                     I2C/UART/SPI（SDA/SCL 默认取自 app_config）
 │    ├─ 选定传感器各 XxxSensor.Start()   经 Board 地址式原语初始化 + 注册到 registry
 │    ├─ 选定屏                        探测屏 → 面板 → LVGL → 构建 UI
 │    └─ 选定链路 new BleLink/WifiLink  板自己决定是 BLE 节点还是 WiFi 节点
 ├─ board_->GetLink()->Start()       启动链路（BleLink 内先 nimble_port_init 再注册 GATT）
 ├─ power_manager.Init()             注册空闲回调（可选）
 └─ xTaskCreate(SensorTask)          1s 周期采集 + 刷新屏
```

### 蓝牙自动配对流程

```
ble 广播（含 esp32-node 服务 UUID）
  └─ hub 主动 GATT 连接
      └─ 握手协议响应（与 esp32-hub 对应）
          ├─ 收到 hub 握手请求 { relay_id, version }
          ├─ 上报握手响应 { node_id, sensor_types[], version }
          ├─ 收到 hub 接受配对 { accepted: true, report_interval_ms }
          └─ 发送配对确认 { paired: true }
              └─ 保存 hub_id 到 NVS
              └─ 启动 BLE 采集任务定时采集
```

### 传感器数据上报流程

```
ble 采集任务定时触发
  └─ sensor_registry.ReadAll() 采集（遍历所有已注册传感器）
      └─ 数据打包（按传感器类型格式化）
          └─ tx 队列 → 发送任务 GATT Notify 上报
              └─ 等待下一周期
```

### 断线重连流程

```
GATT 连接断开
  └─ ble 重新广播
      └─ 等待 hub 重新连接（hub 端也会自动重连已配对节点）
          └─ 重新握手（使用保存的 hub_id 快速配对）
              └─ ble 恢复采集
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
`unit`（显示单位）、`decimals`（小数位）。OLED 显示实现（`ThpDisplay` / `VocDisplay`）遍历这些描述符自动生成仪表盘行，
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

每个传感器类型对应一个数据包，由 `ble` 按注册表格式打包，`sensor_pipeline`（hub 端）按 `type` 解析后上报 MQTT。
单位：温度 ℃、湿度 %RH、气压 hPa、海拔 m（海拔以 `app_config.sea_level_hpa` 为参考海平面气压换算）。

## 显示（display）

显示抽象是 `Display`（`main/display/display.hpp`），只暴露「更新什么」，不暴露「用什么屏」：

- `UpdateSamples(samples, n)`：推入一批传感器读数（`Application::SensorTask` 每 1s 调用）；
- `SetStatus(text)`：设置页眉状态文本（来自当前 `Link::StatusText()`）；
- `ShowNotification()` / `SetBacklight()` / `Lock()` / `Unlock()`：提示、背光与并发保护。

具体实现随板放在 `main/boards/<项目>/`，与板一起编译：

- `ThpDisplay` / `VocDisplay`（0.96" SSD1315，128×64 单色，I2C 地址 `0x3C`/`0x3D` 上电自动探测，两个 C3 板使用）；
- `FanDisplay`（ILI9341 240×320 SPI TFT + XPT2046 触摸，s3_tft_fan 板使用，内含多页触摸 UI 与风扇开关）；
- `NoDisplay`（`main/display/no_display.hpp`）：屏探测/初始化失败时的兜底实现，`Board::GetDisplay()` 因此永不返回 `nullptr`。

各板实现自行组合「驱动 + 内容」（例如 `FanDisplay` 内既有 ILI9341 面板/LVGL 初始化，也有多页触摸 UI），
对上层只体现为统一的 `Display` 接口。

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
- 取数：直接调 `sensor_registry.ReadAll()` 拿实时值，不走 BLE 采集任务——后者只在配对后采样，而屏上未配对时也要显示。
- 可选性：项目未选中的显示屏，其驱动源文件整体不编入；屏未接或初始化失败时，板装配层仅打告警并切到 `NoDisplay`，节点照常广播/上报。

## 项目配置机制与硬件扩展

「项目即宏」在 `idf.py menuconfig` → **Node project → Project profile** 里选一个项目
（定义于 `main/Kconfig.projbuild`，编译期决定，零运行时开销）：

| 层级 | Kconfig 符号 | 性质 | 消费方 |
| :--- | :--- | :--- | :--- |
| 项目（用户唯一入口，单选 choice） | `CONFIG_NODE_PROJECT_*` | 三个项目之一，默认 `S3_TFT_FAN` | `main/CMakeLists.txt` 映射到 `boards/<name>/` 目录 |
| 内部开关（无 prompt，被项目 `select` 锁定） | `CONFIG_NODE_SENSOR_*` | 传感器驱动条件编入 | `main/CMakeLists.txt` |
| 内部开关（同上） | `CONFIG_NODE_DISPLAY_*` / `CONFIG_NODE_TOUCH_XPT2046` | 屏/触摸相关代码条件编入 | `main/CMakeLists.txt`（字体）与 `boards/s3_tft_fan/fan_display.*` 的 `#if` |

项目间除 Kconfig 外的另一部分差异（目标芯片、BT/PSRAM/LVGL/分区表等 sdkconfig 项）
放在各项目 `config.json` 的 `builds.sdkconfig_append` 中，由 `release.py` 构建时生成
`build/node-build.sdkconfig.defaults` 片段拼接到根 `sdkconfig.defaults` 之后。

### 加一个新传感器（以 I2C 传感器为例）

1. 在 `main/sensors/` 加 `XxxSensor.hpp/.cpp`，继承 `SensorDevice`：实现 `Type()`（类型标识）
   与 `Start(Board&, AppConfig&, SensorRegistry&)`——在里面用 `board.I2cWrite()` / `board.I2cWriteRead()`
   （或 `board.UartRead()`）访问总线、初始化芯片，调 `registry.Register()` 登记类型/format_json/**字段描述符**/ReadThunk。
   驱动只保存 `Board* board_`，不持有任何 ESP-IDF 句柄。
2. `main/CMakeLists.txt`：加 `if(CONFIG_NODE_SENSOR_XXX)` 把新 cpp 加入 `SRCS`（REQUIRES 不动）。
3. `main/Kconfig.projbuild` 的内部开关区加一个无 prompt 的 `config NODE_SENSOR_XXX bool`。
4. 在使用该传感器的板 `<Xxx>Board.cpp` 里实例化：`static XxxSensor xxx; xxx.Start(*this, …)`，
   并在该项目的 `config NODE_PROJECT_*` 下加一行 `select NODE_SENSOR_XXX`。若该板尚未实现所需原语，
   在 `<Xxx>Board.hpp/.cpp` 里 override 对应方法（I2C 各板已有通用实现，UART 见 `c3_oled_voc`）。

完成后：握手清单、周期采集、仪表盘行（自动多一行）全部自动生效，其他模块零改动。

### 加一种新显示屏

1. 在 `main/boards/<项目>/` 加 `XxxDisplay.hpp/.cpp`，继承 `Display`：在构造/初始化里建面板、
   挂 LVGL 并构建 UI，实现 `UpdateSamples()` / `SetStatus()` 等刷新接口。
2. `main/CMakeLists.txt` 在对应项目的 `elseif(CONFIG_NODE_PROJECT_<NAME>)` 分支里把新 cpp 加入 `SRCS`；
   `main/Kconfig.projbuild` 内部开关区加 `config NODE_DISPLAY_XXX bool`。
3. 在该项目 `<Xxx>Board.cpp` 里创建新屏对象存入 `display_`，并在其项目宏下 `select NODE_DISPLAY_XXX`。

### 加一个全新项目

1. 新建 `main/boards/<新项目>/`：
   - `config.h`：引脚/通道等编译期常量；
   - `<Xxx>Board.hpp/.cpp`：装配实现（建总线、实例化传感器与屏、创建 `Link`，可从相近项目复制裁剪）；
   - `<Xxx>Display.hpp/.cpp`：本板显示实现（如需要）；
   - `config.json`：`target`（esp32c3/esp32s3）+ `builds[0].sdkconfig_append`
     （首行必须是 `CONFIG_NODE_PROJECT_<宏名>=y`，其余为该项目的 sdkconfig 项）。
2. `main/Kconfig.projbuild` 的 `NODE_PROJECT` choice 加一个 `config NODE_PROJECT_<NAME>`，
   用 `select` 锁定所需内部开关。
3. `main/CMakeLists.txt` 加一个 `elseif(CONFIG_NODE_PROJECT_<NAME>) set(BOARD_DIR "boards/<目录>")`，
   并把该板的 `<Xxx>Board.cpp` / `<Xxx>Display.cpp` 加入 `SRCS`。
4. `python scripts/release.py <新项目>` 构建验证。

> 新建项目时按需在 `<Xxx>Board.cpp` 内创建自己的 `Link`（BLE 板用 `BleLink`，WiFi 板用 `WifiLink`），
> `Application` 无需改动。

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
项目默认映射见 `main/boards/c3_oled_*/config.h`）：

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

完整引脚见 `main/boards/s3_tft_fan/config.h`。

## 构建与烧录

在**已初始化的 ESP-IDF shell** 中（Windows 用 ESP-IDF PowerShell/CMD 快捷方式，
Linux 先 `. $IDF_PATH/export.sh`），于仓库根目录执行：

```bash
python scripts/release.py --list                    # 列出全部项目
python scripts/release.py c3_oled_thp               # 配置 + 编译
python scripts/release.py s3_tft_fan --flash -p COM7         # 编译 + 烧录
python scripts/release.py c3_oled_voc --flash --monitor -p COM7  # 烧录 + 串口监视
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
> 不会自动应用，需参照对应 `main/boards/<项目>/config.json` 的 `sdkconfig_append` 自行勾选。
>
> `display` 与 `boards/s3_tft_fan` 依赖 `lvgl/lvgl` 与 `espressif/esp_lvgl_port`（组件管理器的 managed component），
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
