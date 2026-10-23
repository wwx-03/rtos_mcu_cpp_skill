---
name: mcu-rtos-task-loop
description: 'RTOS MCU 工程（FreeRTOS / CMSIS-RTOS2 / ESP-IDF / STM32 / ESP32 / 嵌入式 C++）的新建、改造与审查：模块分层归属（devices / services / bsp / app / utility / port）、命名词典与代码风格、中断→队列→任务链路、Application MainEventLoop、APP 异步 API（DrawXxx → Post → Handle → HandleXxx）、依赖注入、生命周期与失败降级。Use when adding tasks, queues, semaphores, timers, ISR-to-task paths, or app modules with async APIs; when deciding whether a module belongs in devices or services; or when reviewing naming, layering, and RTOS concurrency in a FreeRTOS / CMSIS-RTOS2 / ESP-IDF project.'
argument-hint: 'MCU/RTOS/SDK 版本 + 要新增或改造的模块，例如"ESP32-C3 + ESP-IDF 6.0，加一个 MQTT 上报模块"'
---

# RTOS MCU 工程（平台无关核心）

**先判层，再命名，再写并发。** 平台差异只允许出现在 `port/`。

## 0. 默认架构：Hybrid

- 网络、MQTT、存储、时间、OTA → `services` 接口；显示、传感器等器件 → `devices` 接口；
  两者都由 `bsp` 适配。
- 简单稳定器件（GPIO 锁、蜂鸣器、LED）→ `app` 可直接使用 `bsp` 具体类。
- **默认 Hybrid**；只有用户明确要求时才用 Layered 或 DIP。三种模式的目录、Board 形态
  与构建约束见 [architecture.md](./references/architecture.md)。

## 1. 八条不变量

1. **中断只做三件事**：清标志、搬数据、投递；只用 `port::QueueSendFromIsr` /
   `port::SemaphoreGiveFromIsr` / `port::TaskNotifyGiveFromIsr`，末尾
   `port::YieldFromIsr(woken)`。ISR 里不打印、不延时、不阻塞、不分配、不拿互斥量。
2. **`Init()` 建资源，`Start()` 建任务**：队列/信号量/定时器在 `Init()` 创建，
   任务在 `Start()` 创建；运行期不 create/delete。
3. **`Application::Start()` 不启动调度器**：调度器只由平台入口（`main()` →
   `port::StartScheduler()`）调用一次；ESP-IDF 下为空实现。
4. **APP 公有 API 只投递**：校验 → 拷贝 → 入队（timeout = 0）→ 返回；
   返回值只表示“是否入队”，不表示“是否完成”。
5. **一个并发域一个任务**：按资源 owner 拆，不按类、不按外设拆；
   低频多实例共享软件定时器。
6. **依赖构造注入**：`Board::GetInstance()` 只出现在组合根，不进业务逻辑，更不进回调。
7. **回调只投递**：厂商事件、定时器、ISR 回调里不 stop/free、不解析协议、不写 Flash。
8. **平台差异只在 `port/`**：`app` 不出现 `#ifdef ESP_PLATFORM` / `HAL_*` / `esp_*`。

## 2. 生成流程（强制顺序）

先判断任务类型：

- **新建**：按下面 1→6。
- **改造**：先按 [layers-and-naming.md](./references/layers-and-naming.md) §7 盘点并输出
  “现有 → 标准”映射表，再做最小 diff。
- **审查**：不写代码，按 §8 输出分级偏差清单。

落地步骤（判层 → 目录 → port → 接口 → bsp → app → 事件流 → 验收）见
[project-workflow.md](./references/project-workflow.md)。

1. **识别**：MCU、核数、RTOS、SDK 版本、构建系统、现有工程结构。
2. **判层**：按 §3 给每个模块定层，填“模块 → 层 → 接口 → 实现 → 任务”表。
3. **填契约**：完成 [platform-profile.md](./references/platform-profile.md) 的 20 项，
   写完再动代码。
4. **顺序**：`port/` → `devices`/`services` 接口 → `bsp/` → `app/`；
   一次只做一个垂直切片，做完编译一次。
5. **命名与风格**：按 §4；与现有工程冲突时以 §4 为准。
6. **交付**：按 §6 输出契约给计划和结果。

不确定就查 SDK 头文件；查不到就问用户，或标 `TODO` 并写明假设。**禁止猜。**

## 3. 分层归属（先判层，再写代码）

| 层 | 放什么 | 判据 | 禁止 |
| --- | --- | --- | --- |
| `devices/` | 器件抽象、纯协议/算法接口 | 有芯片型号/数据手册，通过总线读写寄存器或固定命令 | RTOS、厂商 SDK、网络、存储、时间、任务/队列 |
| `services/` | 平台能力接口 | SDK 提供，有连接/状态/凭据/重试/异步事件，跨器件 | 具体引脚、具体芯片驱动、业务报文 |
| `bsp/` | 具体驱动 + 板级组合根 | 绑定引脚、总线、中断、厂商回调 | 业务状态机、上层类型 |
| `app/` | 业务状态机、协议报文、事件派发 | 规则/流程/策略，换硬件不变 | 寄存器、HAL/IDF 调用、平台 `#ifdef` |
| `utility/` | 纯函数、数据结构 | 无 IO、无时间、无 RTOS | 全局状态、硬件依赖 |
| `port/` | RTOS/SDK 薄封装 | 会随平台变化的语义 | 业务逻辑 |

**device 与 service 的分界线**：`devices` 是“一个器件”，`services` 是“一种平台能力”。

- WiFi / MQTT / HTTP / NVS / SNTP / OTA / 加密 → `services`；
- 屏幕 / 传感器 / 按键 / 继电器 / ADC / 电机 → `devices`；
- **只要有网络、存储、时间或 OTA，就必须建 `services/`，不允许塞进 `devices/` 或 `app/`。**

判定顺序、模块归类表、常见误判见 [layers-and-naming.md](./references/layers-and-naming.md)。

## 4. 命名与风格（唯一标准）

| 对象 | 规则 | 示例 |
| --- | --- | --- |
| 目录 | snake_case，固定六层 | `app/ bsp/ devices/ services/ utility/ port/` |
| 文件 | snake_case，一名一类 | `water_monitor.hpp` / `water_monitor.cpp` |
| 类/结构体 | PascalCase，接口不加 `I` 前缀 | `WaterMonitor`、`Display`、`Ssd1306Panel` |
| 具体板 | `<Mcu><BoardName><Version>Board`，只在 `.cpp` 内定义 | `Esp32SmartLockV01Board` |
| 方法 | PascalCase，动词开头 | `DrawText()`、`HandleRxCpltEvent()` |
| 自由函数 | PascalCase（`create_new_board()` 固定例外） | `ToTicks()` |
| 数据成员 | snake_case + 后缀 `_` | `queue_`、`ready_` |
| 句柄 | 类型后缀 | `main_queue_`、`led_task_`、`bus_mutex_`、`key_timer_` |
| 常量 | `constexpr kPascalCase` | `kMaxSamples` |
| 宏 | `UPPER_SNAKE`，仅限条件编译 | `CONFIG_*` |
| 事件 | `enum class XxxEvent : uint32_t`，值 `kXxx` | `kShowText` |
| 生命周期 | 只允许 `Init` / `Start` / `Stop` | 不用 `Begin` / `Setup` / `Run` / `Open` |
| 公有异步 API | 动词 + 名词，返回 `bool`（是否入队） | `DrawText()`、`Publish()` |
| 内部投递 | `PostXxx()` | `PostWaterQualityEvent()` |
| 任务入口 | `static void TaskEntry(void *ctx)` + `TaskLoop()` | |
| 只读查询 | `IsXxx()` / `HasXxx()`，只读状态位 | `IsConnected()` |

格式：Tab 缩进、K&R、`#pragma once`、`nullptr`、`static_cast`、不用 C 风格转换；
头文件不写 `using namespace`；注释中文，只写非显然的“为什么”。
格式模板见 [templates/style/](./templates/style/README.md)（`.clang-format` / `.editorconfig`）。

**改造现有工程时**：新文件和新改的符号一律按本表命名；现有不一致的命名**不批量重命名**，
在报告里列“偏差清单”。现有工程的命名只用于读懂语义，**不作为新代码的命名依据**；
不允许用“跟现有工程保持一致”作为沿用错误命名的理由。

## 5. 平台契约（20 项）

| 项 | 典型值 |
| --- | --- |
| 架构模式 | Hybrid（默认）/ Layered / DIP |
| 入口函数 / 谁启动调度器 | `main()` / `app_main()` |
| 调度器是否已启动 | 是 / 否 |
| 任务栈单位 / 高水位单位 | byte / word |
| 静态分配支持 | 支持 / 不支持 |
| ISR 投递 API / yield | `...FromIsr` + `portYIELD_FROM_ISR` |
| 中断优先级约束 | NVIC 数值 / IDF 级别 |
| 临界区语义 | 单核关中断 / 跨核 spinlock |
| 定时器回调上下文 | task / ISR |
| 厂商事件回调上下文 | task / ISR |
| 分配策略 | 静态 / `Init()` 动态 / 池化 |
| STL / `std::function` 策略 | 允许 / 禁止 |
| 异常 / RTTI | 开 / 关 |
| 构建系统 / 组件依赖 | CMake / Keil / EIDE |
| include 约定 | `freertos/FreeRTOS.h` 等 |
| 分区 / Flash 限制 | |
| 验证命令 | build / size / 启动日志 |
| 模块矩阵 | `docs/module-matrix.md` |
| 主机侧测试 | 有 / 无 |
| 板级 `.cpp` 数量 | 必须为 1 |

完整模板与查证方法见 [platform-profile.md](./references/platform-profile.md)。

## 6. 输出契约

**写代码前**先给 ≤10 行计划：

1. 判层表：模块 → 层 → 接口文件 → 实现文件；
2. 本次新增/修改的文件清单；
3. 假设与待确认项。

**写代码时**：

- 只改与本任务相关的文件；不重命名无关代码，不调整无关格式；
- 不生成 `README.md`、总结、设计文档，除非用户明确要求；
- 新增代码 100% 按 §4 命名；
- APP 模块统一按 `DrawXxx() { PostXxx() } → TaskLoop → Handle(event) → HandleXxx()` 写，
  事件类型与消息结构私有；
- 一个文件一个类；头文件只暴露必要接口；
- 优先复用 `templates/` 的结构，不重新发明目录。

**写完后**：

- 能编译就编译；不能编译就写清楚缺什么；
- 自查：新增符号是否符合 §4、模块是否落在 §3 的层、`Init/Start` 是否分离；
- 报告不超过 10 行：改了哪些文件、关键决策、未验证项。

**审查任务**：

- 不写代码；按 [layers-and-naming.md](./references/layers-and-naming.md) §8 输出
  “文件:行 / 类型 / 问题 / 建议 / 严重度”，先 P0 后 P1；
- 只给最小修改建议，不给全量重命名清单。

## 7. 并发骨架（与 `templates/app` 一致）

组合根顺序：`Board::GetInstance().Init()` → `Application::Init()` → `Application::Start()`
→ 平台入口启动调度器（CubeMX）。

`Application` 是组合根：持有 `Board &` 和子 APP，事件类型私有；`Init()` 建资源，
`Start()` 启动子 APP/主任务/timer，`MainEventLoop()` 只派发。

```cpp
class Application {
public:
	static Application &GetInstance();
	bool Init();                     // 队列 / timer / 子 APP 的 Init
	bool Start();                    // 子 APP Start + 主任务 + timer Start
	uint32_t DroppedCount() const;

private:
	struct Event { ... };            // 事件类型与消息结构私有
	static void TimerEntry(void *ctx);
	static void TaskEntry(void *ctx);
	void MainEventLoop();
	void HandleEvent(const Event &event);
	bool Post(const Event &event);

	Board &board_;
	Display display_;
	// 静态：StaticQueue/StaticTask/StaticTimer
	// 动态：QueueHandle/TaskHandle/TimerHandle
	std::atomic<uint32_t> dropped_{0};
};
```

每个 APP/并发域自己持有队列与任务，公有 API 只投递：

```cpp
class Display {
public:
	bool Init();
	bool Start();
	bool DrawText(int x, int y, const char *text);   // 异步：返回是否入队
	bool DrawValue(int x, int y, int32_t value);     // 异步
	bool Clear();

private:
	enum class Event { kDrawText, kDrawValue, kClear };
	struct Message { Event event; ... };

	static void TaskEntry(void *ctx);
	void TaskLoop();
	void Handle(const Message &message);
	void HandleDrawText(const Message &message);
	void HandleDrawValue(const Message &message);
	void HandleClear();
	bool Post(const Message &message);
};
```

链路固定为：

```text
DrawXxx() { PostXxx() } -> 内部队列 -> TaskLoop()
    -> Handle(event) -> HandleXxx() { 格式化 / 写 panel / 更新状态 }
```

- `DrawXxx()` 只做校验、拷贝、入队；**不直接操作 panel/驱动**。
- 事件枚举与消息结构私有；外部只能看到 `DrawXxx()`。
- 小 RAM 用 `port::StaticTask` / `StaticQueue` / `StaticTimer`；大 RAM 用
  `port::CreateTask` / `CreateQueue` / `CreateTimer`（无模板）。
  两套完整模板见 [templates/app/](./templates/app/README.md)。
- 主循环 handler 必须快速返回：只更新状态和投递，重活交给 APP 任务。
- 模块内部事件走各自队列；主队列只放中断事件与跨模块事件。
- 队列满了计数，不静默丢弃；队列元素保持小，大块数据传索引/槽位。

并发细节见 [concurrency-rules.md](./references/concurrency-rules.md)、
[callback-contexts.md](./references/callback-contexts.md)、
[timer-vs-task.md](./references/timer-vs-task.md)、
[freertos-port-semantics.md](./references/freertos-port-semantics.md)。

## 8. 高频错误

| 错误 | 正确 |
| --- | --- |
| WiFi/MQTT/NVS 放进 `devices/`，或干脆没有 `services/` | 平台能力一律 `services` 接口 + `bsp` 适配 |
| APP 内部 `Board::GetInstance()` | 构造函数注入 |
| 每个外设/每个类一个任务 | 一个并发域一个任务 |
| 所有事件塞进 `MainEventLoop` | 模块内部事件走各自队列 |
| `MainEventLoop` handler 做重活 | 只更新状态 + 投递 |
| 用 `vTaskDelay` 做周期 | `port::DelayUntil` |
| `while (!flag) {}` 忙等 | 阻塞在队列/信号量/通知 |
| 栈大小按 word 猜 | 平台契约 + `stack_bytes` + 高水位实测 |
| 运行期反复 `new` / create / delete | `Init()` 一次分配 |
| 公有 API 直接写 panel/驱动 | 只入队，绘制/写入在 `HandleXxx()` |
| 回调里直接绘制/解析 | 回调只投递 |
| 离线数据无限缓存、日志打印 token | 有上限、脱敏 |

失败降级、可观测性、时间与连接状态机见
[runtime-policies.md](./references/runtime-policies.md)；分配与 STL 策略见
[memory-and-stl.md](./references/memory-and-stl.md)。

## 9. 索引

| 文件 | 用途 |
| --- | --- |
| [layers-and-naming.md](./references/layers-and-naming.md) | 分层判据、模块归类、命名/风格/改造规范 |
| [project-workflow.md](./references/project-workflow.md) | 按这套模式写工程的落地步骤 |
| [architecture.md](./references/architecture.md) | Layered / DIP / Hybrid、Board 形态、构建强制 |
| [concurrency-rules.md](./references/concurrency-rules.md) | 任务/队列/优先级/同步原语/生命周期 |
| [platform-profile.md](./references/platform-profile.md) | 平台契约模板与查证方法 |
| [freertos-port-semantics.md](./references/freertos-port-semantics.md) | FreeRTOS / ESP-IDF / CMSIS-RTOS2 语义差异 |
| [esp-idf.md](./references/esp-idf.md) | ESP-IDF 启动、组件、IDF 6 迁移 |
| [stm32-cubemx.md](./references/stm32-cubemx.md) | STM32/CubeMX、HAL、NVIC、EIDE/Keil |
| [callback-contexts.md](./references/callback-contexts.md) | ISR / 厂商事件 / 定时器上下文 |
| [timer-vs-task.md](./references/timer-vs-task.md) | 周期任务 vs 软件定时器 |
| [memory-and-stl.md](./references/memory-and-stl.md) | 分配策略、STL / `std::function` 风险 |
| [runtime-policies.md](./references/runtime-policies.md) | 失败降级、可观测性、时间、连接状态机 |
| [module-matrix.md](./references/module-matrix.md) | 逐模块依赖与任务预算 |
| [product-architecture.md](./references/product-architecture.md) | OTA/低功耗/看门狗/NVS/安全 |
| [bring-up.md](./checklists/bring-up.md) | 唯一一份验收清单 |
| `templates/` | port 实现与平台工程骨架 |
| [templates/app/](./templates/app/README.md) | 静态/动态任务创建模板（同一接口，二选一） |
| [templates/style/](./templates/style/README.md) | `.clang-format` / `.editorconfig`，格式落地 |
| `examples/` | 采集→显示→MQTT 案例 |