# 按这套模式写工程

本文是落地顺序；规则细节见 `SKILL.md`、`references/layers-and-naming.md`、
`references/architecture.md`。目标是：**先定层和接口，再写 port/bsp，最后写 app**，
不要从 `Application` 开始堆代码。

## 0. 最终产物

```text
<project>/
├── components/
│   ├── port/                  # 唯一 RTOS/SDK 依赖点
│   ├── devices/               # Board / Device / DisplayPanel 接口
│   ├── services/              # 网络/存储/时间/OTA 接口
│   ├── bsp/                   # 具体板 + 驱动实现
│   ├── app/                   # Application + APP 模块
│   └── utility/               # 纯逻辑（滤波/CRC/格式化）
├── main/                      # 入口：main.c / app_main
├── docs/
│   ├── platform.md            # 平台契约
│   └── module-matrix.md       # 模块 → 层/接口/任务/队列
└── tests/                     # 主机侧测试 + mock
```

## 1. 列模块、判层

先填这张表，不写代码：

| 模块 | 层 | 接口文件 | 实现文件 | 归属任务 | 分配 |
| --- | --- | --- | --- | --- | --- |
| 例：Display | `app` | `app/display.hpp` | `app/display.cpp` | `display` | 静态/动态 |
| 例：DisplayPanel | `devices` | `devices/display_panel.hpp` | `bsp/ssd1306_panel.cpp` | 调用者 | - |
| 例：WiFi | `services` | `services/network.hpp` | `bsp/wifi_station.cpp` | `network` | init 动态 |

判定顺序见 `layers-and-naming.md` §2。特别注意：

- 网络 / MQTT / NVS / SNTP / OTA / 加密 → `services`，不要塞进 `devices`。
- 有芯片、总线、数据手册 → `devices`；实现放 `bsp`。
- 业务规则 / 协议报文 / 状态机 → `app`。
- 只被 `app` 使用的纯算法 → `utility`。

## 2. 选分配策略

按模块选，不要求全工程统一：

| 条件 | 选择 |
| --- | --- |
| RAM 紧、产品固定、无 PSRAM | `port::StaticTask` / `StaticQueue` / `StaticTimer` |
| RAM 充足、需要按配置调整 | `port::CreateTask` / `CreateQueue` / `CreateTimer` |

同一个模块只用一种风格；静态模块的对象必须是全局/静态或由 `Board` 持有。

## 3. 填平台契约

完成 `references/platform-profile.md` 的 20 项，写入 `docs/platform.md`：

- 入口 / 调度器是否已启动；
- 栈单位 / 高水位单位；
- 静态分配支持；
- ISR API / 中断优先级；
- 临界区是否跨核；
- timer / 厂商回调上下文；
- 构建系统 / 分区限制。

不确定就查 SDK 头文件；查不到问用户或标 `TODO`，禁止猜。

## 4. 写 port

- 复制 `templates/port/rtos_port.hpp` + 目标平台的 `port/rtos_port.cpp`。
- `port` 只做 RTOS/SDK 薄封装，不放业务逻辑。
- 分别用 `configSUPPORT_STATIC_ALLOCATION=1/0` 编译一次。
- `StaticTask` / `StaticQueue` / `StaticTimer` 的 control 大小用 `static_assert` 校验。

## 5. 写 devices / services 接口

- 纯接口，不 include RTOS / 厂商头。
- `Board` 至少提供 `Init()`、`GetDevice()`、`GetDisplayPanel()`（按项目增减）。
- 具体板只在 `bsp/<board>.cpp` 内定义，用 `IMPLEMENT_BOARD_CLASS` 注册。
- 实现层头文件放 `src/`，用 `PRIV_INCLUDE_DIRS` 私有化。

## 6. 写 bsp

- 实现 `devices` / `services` 接口，只做 IO、总线事务、ISR、厂商回调。
- ISR 只清标志、搬数据、投递；厂商回调只投递。
- `Device::SetReceivedCallback()` 的回调必须在任务上下文调用；
  ISR 里先投递到 BSP 任务，由 BSP 任务调用 `NotifyReceived()`。
- 总线事务有超时、锁/恢复策略；启动时 probe。

## 7. 写 Application

- 事件枚举与消息结构 **private**。
- `Init()`：创建主队列、timer、子 APP 的 `Init()`。
- `Start()`：启动子 APP、创建主任务、启动 timer、投递 `kStart`。
- `MainEventLoop()`：只 `HandleEvent()`，不解析协议、不写 Flash。
- 失败可见：`Init()` / `Start()` 返回 bool，失败记录并决定降级/停机。

## 8. 写 APP 模块（以 Display 为例）

固定链路：

```text
DrawText() / DrawValue() / Clear()
        │  PostXxx()（timeout = 0）
        ▼
APP 内部队列
        ▼
TaskLoop()
        ▼
Handle(event)
        ├─ HandleDrawText()  → panel_->ShowText()
        ├─ HandleDrawValue() → snprintf + panel_->ShowText()
        └─ HandleClear()     → panel_->Clear()
```

规则：

- 公有 API 只做参数校验、拷贝、入队；返回“是否入队”。
- 事件枚举、消息结构、`Post`、`Handle`、`TaskLoop` 全部 private。
- 只有 `HandleXxx()` 可以访问 panel/驱动。
- 队列满 `dropped_` 计数，不阻塞调用者。
- `Init()` 建队列，`Start()` 建任务；运行期不 create/delete。

## 9. 事件流

| 来源 | 入口 | 注意 |
| --- | --- | --- |
| 硬件 ISR | `...FromIsr` + `YieldFromIsr(woken)` | 不打印、不延时、不分配 |
| FreeRTOS timer | 回调里 `PostTimerElapsed()` | 只投递，不阻塞 |
| 厂商事件任务 | `PostXxx()` | 不做重活 |
| Device 回调 | BSP 任务里 `NotifyReceived()` | 回调必须在任务上下文 |
| APP 之间 | 对方公有异步 API | 不直接调用内部方法 |

## 10. 构建与验收

1. `port` + `devices` + `bsp` + `app` 分组件编译。
2. 从无 `sdkconfig` 的全新构建通过；条件编译分支分别构建。
3. `size` / 分区余量、栈高水位、heap 高水位。
4. `checklists/bring-up.md` 逐项核对。
5. 纯逻辑（协议、滤波、状态机）尽量在主机侧用 mock 测试。

## 11. 新增一个 APP 模块的检查表

- [ ] 已判层，接口/实现/任务写进 `docs/module-matrix.md`
- [ ] 事件枚举与消息结构 private
- [ ] 公有 API 只投递，返回 bool
- [ ] `Init()` 建队列，`Start()` 建任务
- [ ] `TaskLoop() → Handle(event) → HandleXxx()` 链路完整
- [ ] 队列满/失败有计数
- [ ] 任务栈/优先级/周期已记录并实测
- [ ] 命名符合 `layers-and-naming.md` §5
- [ ] 失败路径明确（降级或停机）
