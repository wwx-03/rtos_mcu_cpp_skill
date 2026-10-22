---
name: mcu-rtos-task-loop
description: '编写或改造 RTOS MCU 工程（FreeRTOS / CMSIS-RTOS2 / ESP-IDF / STM32 / ESP32 / 嵌入式 C++）时使用本技能：以平台无关的方式约束中断→队列→任务链路、Application MainEventLoop、APP 异步 API、依赖注入、生命周期与失败降级。生成代码前必须先填写平台契约（入口与调度器、栈单位、ISR/临界区语义、回调上下文、构建系统），在 Layered / DIP（Ports & Adapters）/ Hybrid 三种架构模式中选择一种并用构建系统强制依赖方向，再按目标 MCU/SDK 生成 port/ 实现。适用于新建 RTOS 工程、新增 APP 模块/任务/队列/软件定时器、审查 ISR/事件循环/任务/内存分配、选择 app→bsp→devices 还是 app→devices←bsp 的工程结构，以及 ESP-IDF 与 STM32/CubeMX 的工程配置。Use when adding tasks, queues, semaphores, timers, ISR-to-task paths, or app modules with async APIs in a FreeRTOS/CMSIS-RTOS2 project, when choosing between layered and dependency-inverted device abstractions, and when generating platform-specific code from a platform contract.'
argument-hint: 'MCU/RTOS/SDK 版本 + 要新增的 APP 模块 / 任务 / 数据流，例如"ESP32-C3 + ESP-IDF 6.0，加一个 MQTT 上报模块"'
---

# RTOS 多任务工程模式（平台无关核心）

本技能只约束**工程结构与并发边界**，不绑定具体 MCU、厂商 SDK 或 RTOS API：

- 中断只把数据/事件投递给队列；
- `Application` 用 `MainEventLoop()` 统一派发中断事件与跨模块事件；
- 每个 APP/并发域自己持有队列和任务，对外只暴露**异步 API**；
- 依赖在构造时由 `Board` 注入，APP 内部不做全局查找；
- **生成代码前先选定 Layered / DIP / Hybrid 架构模式**，记录到平台契约，并用构建系统强制依赖方向；
- **所有平台差异必须显式写进「平台契约」**，再由 `port/` 落地。

> 平台细节放在 `references/`，代码骨架放在 `templates/`。生成代码前先读
> `references/platform-profile.md`，完成平台契约，再开始写代码。
>
> **范围说明**：本 skill 的核心是 RTOS 并发、回调和分层纪律，不是完整产品架构方法论。
> OTA/分区、低功耗、看门狗、NVS 迁移、安全、离线缓存等见
> `references/product-architecture.md`；逐模块依赖与任务预算见 `references/module-matrix.md`。

## 0. 使用顺序（强制）

1. 从需求/仓库识别：MCU、核数、RTOS、SDK 版本、构建系统、外设。
2. 选择架构模式（Layered / DIP / Hybrid，见第 2 节），记录选择理由与模块边界，
   并填写 `references/module-matrix.md` 的模块路径、任务预算与分配策略。
3. 阅读目标 SDK 的头文件与文档，填写 `references/platform-profile.md` 中的契约。
4. 先实现 `port/`（唯一平台依赖点），再按选定模式写 `bsp/`、`devices/`、`app/`。
5. 用构建系统强制依赖方向，按「检查清单」做语义检查，再编译、看 size、跑启动日志。
6. 把契约、架构模式与硬件假设记录到项目 `docs/platform.md` 与 `README.md`。

**不确定的语义不许猜。** 查不到就向用户确认，或标 TODO，不允许带着假设生成代码。

## 1. 平台契约（生成代码前的硬门槛）

至少确认下面这些项。完整模板和查证方法见 `references/platform-profile.md`。

| 契约项 | 为什么必须确认 | 典型取值 |
| --- | --- | --- |
| 架构模式 | 决定 app 依赖 bsp 还是 devices/services，以及 Board 的公开边界 | Layered / DIP / Hybrid |
| MCU / 核数 | 决定临界区、任务亲和性、内存约束 | 单核 / 双核 |
| RTOS / 版本 | 决定 API 与栈单位 | FreeRTOS / CMSIS-RTOS2 |
| SDK / 版本 | 决定入口、组件、API 迁移 | ESP-IDF 6.x / CubeMX |
| 入口函数 | 决定谁启动调度器 | `main()` / `app_main()` |
| 调度器是否已启动 | 避免重复 `vTaskStartScheduler()` | 是 / 否 |
| 任务创建 API 栈单位 | word 与 byte 会差 4 倍 | byte / word |
| 栈高水位返回值单位 | 定栈时容易误判 | byte / word |
| 静态分配支持 | 决定小 RAM 平台写法 | 支持 / 不支持 |
| ISR 投递 API / 优先级约束 | 决定 ISR 能调什么 | FromISR API、优先级范围 |
| 临界区语义 | 单核关中断 ≠ 跨核保护 | 关中断 / spinlock |
| 定时器 / 厂商事件回调上下文 | 决定能否阻塞、能否 stop/free | ISR / 任务 |
| 构建系统 / 分区限制 | 决定组件、include、flash 布局 | CMake / Keil / EIDE |

## 2. 分层与依赖方向（含架构模式选择）

### 2.1 三种架构模式

同一套代码可以用三种方式组织依赖。**生成代码前必须选定一种并记录到平台契约**，
不允许在同一个模块里无规则地混用。

#### A. Layered（分层模式）

```text
app → bsp → devices → utility
 │      │
 └──────┴──→ port → RTOS / SDK
```

> `devices` 不应依赖 `port`/RTOS；`port` 由 `app` 和 `bsp` 使用。

- `app` 直接使用 `bsp` 暴露的设备/服务类；`devices` 是 `bsp` 的实现细节。
- 优点：文件少、间接层少、bring-up 快、代码/Flash 占用通常更小。
- 缺点：`app` 与具体 BSP 耦合，换器件/换板/单测时修改面更大。
- 适用：单板、单 MCU、器件固定、小团队、ROM/RAM 紧、无单测/多板需求。

#### B. DIP / Ports & Adapters（依赖倒置）

```text
app → devices / services ← bsp
             ↑
      abstract Board（devices）
             ↑
  Esp32SmartLockV01Board（bsp，按板命名）
```

- `devices/`：纯接口 + 纯协议/算法；**不 include RTOS、厂商头或上层**。
- `services/`：WiFi / MQTT / 存储 / 时间等平台服务接口；不要硬塞进 `devices/`。
- `bsp/`：实现 `devices` / `services` 接口，只做 IO context、ISR、厂商回调、总线事务。
- `app`：只依赖 `devices` / `services` 接口；**不得 include `bsp/` 下任何头**。
- 优点：可 mock/主机单测、可换板/换器件、边界清晰、适合并行开发。
- 缺点：接口/适配器更多、间接层和调试链路更长、代码体积略增。
- 适用：多板/多 MCU、器件可替换、需要主机侧测试、长期维护。

#### Board 的形态（DIP 固定写法）

抽象 `Board` 放在 `devices/board.hpp`，只暴露设备/服务接口：

```cpp
// devices/board.hpp
#pragma once

class Network;
class Display;
class Keypad;
class Lock;
class Board;

// 由具体板的 .cpp 通过 IMPLEMENT_BOARD_CLASS() 提供
Board *create_new_board();

class Board {
public:
	static Board &GetInstance() {
		static Board *instance = create_new_board();
		return *instance;
	}

	Board(const Board &) = delete;
	Board &operator=(const Board &) = delete;

	virtual Network *GetNetwork() = 0;
	virtual Display *GetDisplay() = 0;
	virtual Keypad *GetKeypad() = 0;
	virtual Lock *GetLock() = 0;

protected:
	// 声明拷贝构造会抑制隐式默认构造，必须显式保留
	Board() = default;
	// 静态对象，禁止通过基类指针 delete
	~Board() = default;
};

// 具体板 .cpp 末尾使用一次
#define IMPLEMENT_BOARD_CLASS(class_name) \
	Board *create_new_board() {            \
		static class_name instance;         \
		return &instance;                   \
	}
```

具体板**只在 `.cpp` 里定义，不提供 `.hpp`**：

```cpp
// bsp/esp32_smart_lock_v01_board.cpp
#include "devices/board.hpp"

namespace {

class Esp32SmartLockV01Board final : public Board {
public:
	Esp32SmartLockV01Board();

	Network *GetNetwork() override { return &network_; }
	Display *GetDisplay() override { return &display_; }
	Keypad *GetKeypad() override { return &keypad_; }
	Lock *GetLock() override { return &lock_; }

	// 板级专用能力不放进抽象 Board
	void RefreshWatchdog();

private:
	Esp32MqttNetwork network_;
	Ssd1306Display display_;
	MatrixKeypad keypad_;
	GpioLock lock_;
};

}  // namespace

IMPLEMENT_BOARD_CLASS(Esp32SmartLockV01Board);
```

规则：

1. **命名**：`<MCU><BoardName><Version>Board`，PascalCase，`final`，`override` 显式写出。
2. **抽象 `Board` 只有 `GetXxx()`**，返回 `devices`/`services` 接口指针；不放 HAL 类型、不放板级常量。
3. **板级专用能力**（喂狗、板号、自检）留在具体类，或单独抽服务接口；不要让 app 依赖具体板。
4. **具体板只在 `.cpp` 内定义**，外部只能通过抽象 `Board` 访问；不要为它写 `.hpp`。
5. **生命周期**：`IMPLEMENT_BOARD_CLASS` 用函数内静态对象，不引入 `new` / `malloc`；
   构造必须早于中断使能与任务创建。
6. **选板**：构建时只编译一个 `bsp/*_board.cpp`（多个 → 链接期重复符号，缺失 → undefined reference），
   不需要 `board_select.hpp`，也没有 `#ifdef`。
7. **app 不 include `bsp/`**：`Application` 的构造/`Start()` 接收 `Board &`，内部只用接口。

```cpp
// main.cpp
#include "app/application.hpp"
#include "devices/board.hpp"

extern "C" void app_main(void) {
	// 具体板由 IMPLEMENT_BOARD_CLASS 注册；第一次调用早于任务创建
	Application::GetInstance().Start(Board::GetInstance());
}
```

> 只把 `Board.hpp` 改成返回接口还不够：如果 app 仍然 include 它，app 仍然依赖 BSP。
> DIP 模式下 `Application` 必须由 `main()` 注入 `Board &`（或 `BoardServices`）。
>
> 单例边界：`Board::GetInstance()` 只应出现在组合根 `main()`；为了主机侧测试，
> `Application` 应可构造并注入依赖，不要把 `GetInstance()` 写进业务逻辑。
> devices 接口的 protected 非虚析构适合静态对象；如果需要 mock 或 `unique_ptr`，
> 使用虚析构或显式 deleter，并在测试构建中明确生命周期。

#### C. Hybrid（混合模式）

- 复杂/易变模块（显示、网络、MQTT 传输、存储、指纹、人脸）走 `devices`/`services` 接口。
- 典型拆分：WiFi/MQTT 传输/存储/时间走 `services` 接口 + BSP 适配器；
  JSON 报文、命令分发、业务状态机留在 `app`，不要塞进 BSP。
- 简单稳定模块（GPIO 锁、蜂鸣器、LED）可以直接用 `bsp` 具体类。
- 必须在 `references/module-matrix.md` 中逐模块列出“走哪条路径”，禁止同一模块两条路径并存。
- **构建约束警告**：ESP-IDF 的组件依赖是组件级而非模块级。一旦 `app` `REQUIRES bsp`，
  app 就能 include bsp 的所有公共头文件，无法再按模块强制。真要加强制，必须让
  实现层不暴露公共头（头文件放 `src/` + `PRIV_INCLUDE_DIRS`），或把模块拆成独立组件。
  只在文档里写“逐模块”不算完成。

### 2.2 选择判据（按优先级）

1. **板卡/器件变体**：≥ 2 个板卡/MCU/器件变体，或计划替换器件 → DIP/Hybrid。
2. **可测试性**：需要 mock/主机单测的模块 → DIP/Hybrid。
3. **复杂度**：≥ 3 个复杂或易变模块 → Hybrid/DIP。
4. **团队与生命周期**：多人并行、长期维护、频繁换代 → DIP/Hybrid。
5. **ROM/RAM**：只作为约束/否决项，不是主判据。
   - 极紧（例如 RAM < 32 KB 或 Flash < 128 KB）且无测试/多板需求 →
     Layered/Hybrid，避免接口爆炸。
   - 资源充足不等于必须 DIP；仍按 1~4 选择。
   - 用 `size` / `arm-none-eabi-size`、栈高水位实测，不要凭 MCU 型号猜。

| 场景 | 推荐 |
| --- | --- |
| 单板、单 MCU、固定器件、小团队、快速交付 | Layered |
| 多板/多 MCU、器件可替换、需要单测/mock、长期维护 | DIP |
| 复杂模块多，但简单器件更多 | Hybrid |
| ROM/RAM 极紧且无测试/多板需求 | Layered/Hybrid |

### 2.3 用构建系统强制依赖

以 ESP-IDF 组件为例（其他构建系统用等价的可见性/依赖机制）：

Layered：

```cmake
idf_component_register(... REQUIRES bsp port)                    # app
idf_component_register(... REQUIRES port PRIV_REQUIRES devices)  # bsp
```

DIP：

```cmake
idf_component_register(... REQUIRES devices services port)          # app
idf_component_register(... REQUIRES devices services port)          # bsp
idf_component_register(... REQUIRES app bsp devices services port)  # main
```

Hybrid：

- 逐模块写清楚：抽象模块只出现在 `devices`/`services`；简单模块允许 `app` REQUIRE `bsp`。
  但 REQUIRE `bsp` 会暴露全部 BSP 公共头，只在实现层无公共头或模块独立成组件时才成立。
- 在 `references/module-matrix.md` 和平台契约里维护“模块 → 依赖路径”表，构建系统按表约束。

**验收方法**：临时在 `app` 里 include 一个不允许的层，构建必须失败；
如果仍能编译，说明依赖没有被真正约束。Hybrid 下如果 `app` 为了简单模块
`REQUIRES bsp`，这条验收只对该模块成立，对其他抽象模块不成立；请在
`references/module-matrix.md` 中记录例外。

### 2.4 共同职责边界（三种模式都适用）

- `app/`：Application、各 APP 类（Display / Network / 采集 / 存储）、队列与任务、异步 API。
- `bsp/`：具体驱动、IO context、ISR、厂商回调 → 只投递。实现层默认不暴露公共头；
  驱动/板级头放 `src/`，用 `PRIV_INCLUDE_DIRS` 只对本组件可见。
- `devices/`：器件抽象与纯算法；**不 include RTOS 与厂商头**；不创建任务/队列。
- `utility/`：滤波、格式化、数据结构等纯逻辑。
- `port/`：任务、队列、信号量、临界区、时间、调度器启动的薄封装。
- `app/`、`bsp/` 可以使用 RTOS API，但**平台差异只允许出现在 `port/`**；
  `#ifdef ESP_PLATFORM`、`#ifdef STM32...` 不应散落在 APP 代码里。

`Board` 是板级资源入口/组合根：

- **Layered**：具体 `Board` 单例、私有构造、初始化一次，返回具体设备类。
- **DIP**：抽象 `Board` 只含 `GetXxx()`；具体 `<MCU><BoardName><Version>Board`
  只在 `bsp/<board>_board.cpp` 内定义，末尾用 `IMPLEMENT_BOARD_CLASS()` 注册，
  `main()` 注入 `Application`。
- **Hybrid**：抽象 `Board` 只返回被抽象的设备；简单器件可留在具体板或直接返回具体类。

APP 的依赖全部从构造函数注入，APP 内部不写 `Board::GetInstance().GetXxx()`。
DIP 模式下，`devices/board.hpp` 只暴露 `devices`/`services` 接口，
具体 BSP 类型只在 `bsp/` 与 `main()` 里出现。

### 2.5 反模式

| 反模式 | 正确做法 |
| --- | --- |
| app 同时 include bsp 和 devices，且没有逐模块规则 | 选定 Layered/DIP/Hybrid，用 module-matrix 逐模块写清边界 |
| Hybrid 只在文档写“逐模块”，实现层仍暴露公共头 | 实现头放 `src/` + `PRIV_INCLUDE_DIRS`，或拆独立组件 |
| Board.hpp 暴露具体 bsp 类型，app 直接依赖驱动 | 暴露 devices/services 接口或 BoardServices |
| bsp include app（反向依赖） | app → bsp 或 app → devices ← bsp |
| 在 ISR/高频路径使用捕获式 `std::function`/`std::string` | 函数指针 + context、固定缓冲、池化 |
| devices include RTOS/厂商头 | devices 保持纯抽象，BSP 做适配 |
| ISR 里调用设备协议/业务逻辑/打印/分配 | ISR 只投递，协议与业务在任务里 |
| 为每个 GPIO 都造接口 | 复杂/易变模块才抽象，简单器件可具体类 |
| 只在文档里写模式，没有用构建系统强制 | 用组件/模块依赖做编译期约束 |
| 抽象 `Board` 上出现板级专用方法（喂狗、板号、自检） | 留在具体板或抽成服务接口 |
| 具体板类仍叫 `Board`，与抽象类重名 | `<MCU><BoardName><Version>Board` |
| 多个地方 `#ifdef` 选板 | 只编译一个 `bsp/*_board.cpp`，由链接期唯一符号选板 |
| 为具体板写 `.hpp` 并让 app include 它 | 具体板只在 `.cpp` 内定义，外部只见抽象 `Board` |

## 3. 三条铁律

1. **中断只做三件事，并且只走 ISR 安全 API**
   清标志、搬数据、投递（`port::QueueSendFromIsr` / `port::SemaphoreGiveFromIsr` /
   `port::TaskNotifyGiveFromIsr`），最后处理 woken 并调用 `port::YieldFromIsr(woken)`。
   ISR 里不打印、不延时、不阻塞、不拿互斥量、不分配内存。

2. **资源只在初始化阶段创建一次**
   任务 / 队列 / 信号量 / 定时器在 `Start()` 或 init 路径创建，运行期不 create/delete。
   小 RAM 平台优先静态分配；大 RAM 平台可在 init 阶段动态创建，但必须检查返回值。
   两种情况都禁止：运行期反复 `new` / `malloc`、在热路径用 STL 容器增删。

3. **共享数据只用 RTOS 原语**
   任务之间传数据用队列；保护资源用互斥量（只在任务里获取）；
   只有极短、同核、不与 RTOS API 交叉的临界区才用 `port::EnterCritical()`。
   **临界区是否能跨核保护，由平台契约决定**；双核共享数据必须用 spinlock/原子操作。

## 4. 启动与生命周期

### 4.1 Application 不启动调度器

`Application::Start()` 只创建主任务，**不调用 `vTaskStartScheduler()`**。
启动调度器是平台入口的职责，且只能调用一次：

| 平台 | 入口 | 调度器 | `Application::Start()` |
| --- | --- | --- | --- |
| CubeMX / 裸机 main | `main()` | `port::StartScheduler()` | 只建任务 |
| ESP-IDF | `app_main()` | 已运行，`port::StartScheduler()` 为空实现 | 只建任务 |
| CMSIS-RTOS2 | `main()` | `osKernelStart()` 一次 | 只建任务 |

### 4.2 单例与构造时机

- `Board` / `Application` 的首次构造必须发生在**平台初始化阶段**，不要等到任务或 ISR 里。
- CubeMX：在 `main()` 里、`vTaskStartScheduler()` 之前完成。
- ESP-IDF：`app_main()` 本身已在调度器任务中，在 `app_main()` 里完成即可。
- 函数内 `static` 的首次构造带 `__cxa_guard`；不要在 ISR 里首次构造。

### 4.3 初始化失败策略

- 基础设施失败（总线、内存、调度器）：fail-fast，记录原因并停机/重启。
- 外设失败（屏幕、传感器、网络）：按产品需求选择降级继续或 fail-fast；
  选择降级时，BSP 内部必须有 `ready_` 标志，公有 API 在失败后仍能安全返回。
- 所有失败都必须可见：日志、错误计数、显示状态或诊断接口，不允许静默。

## 5. Application 与 MainEventLoop

职责只有三件：

1. **构造函数**：创建主队列、注册 BSP 回调；不创建任务。
2. **`Start()`**：创建主任务；不启动调度器。
3. **`MainEventLoop()`**：派发中断事件与跨模块事件；`HandleStart()` 里启动各 APP。

```cpp
class Application {
public:
	static Application &GetInstance();
	void Start();

	void PostWifiConnectedEvent();
	void PostWaterQualityEvent(const WaterQualitySample &sample);

private:
	Application();

	static void MainTaskEntry(void *ctx);
	void MainEventLoop();
	void HandleStart();

	port::QueueHandle main_queue_ = nullptr;
	port::TaskHandle main_task_ = nullptr;
	uint32_t dropped_ = 0;

	Display display_;
	Network network_;
	WaterMonitor monitor_;
};

void Application::Start() {
	port::TaskOptions options{"main_loop", 4096, this, 4};
	if (!port::CreateTask(&Application::MainTaskEntry, options, &main_task_)) {
		abort();
	}
	// 不要在这里启动调度器；平台入口负责 port::StartScheduler()
}
```

要点：

- 主循环 handler 必须快速返回：只更新状态和**投递**，重活交给 APP 任务。
- 模块内部事件走各自队列；主队列只放中断事件和跨模块事件。
- 队列满时计数（`++dropped_`），不要静默丢弃。
- 队列元素保持小；低频状态快照（20~50B）可以直接值拷贝，
  高频/大块数据用槽位池、stream buffer 或地址/索引传递。

## 6. APP 类：依赖注入 + 异步 API

每个 APP/并发域是一个自治单元：自己持有队列与任务，对外只暴露“投递后返回”的方法。

```cpp
class Display {
public:
	explicit Display(DisplayPanel &panel) : panel_(panel) {}

	void Start();
	void ShowText(const char *text);              // 异步
	void ShowWaterQuality(const WaterQualitySample &sample);  // 异步

private:
	struct Item {
		uint32_t event;
		WaterQualitySample sample;
		char text[32];
	};

	static void TaskEntry(void *ctx);
	void TaskLoop();
	bool Post(const Item &item);

	DisplayPanel &panel_;
	port::QueueHandle queue_ = nullptr;
	port::TaskHandle task_ = nullptr;
};

void Display::ShowText(const char *text) {
	if (text == nullptr) {
		return;
	}
	Item item = {};
	item.event = SHOW_TEXT_EVENT;
	strncpy(item.text, text, sizeof(item.text) - 1);
	(void)Post(item);   // 队列满就计数丢弃，绝不阻塞调用者
}
```

### 异步 API 的三条规则

1. 公有 API 只做：参数校验、拷贝、入队（timeout = 0），然后返回。
2. 返回值表示“是否入队”，不表示“是否完成”。要结果就注册回调、事件组或状态查询。
3. 队列满时计数，不静默；调试期用 `configASSERT` 或周期日志暴露。

### 什么时候可以同步

- 纯查询、无副作用、返回 POD：`bool IsConnected() const` 可以直接读状态位；
- 不要在公有 API 里等队列、等信号量、写 Flash、阻塞发送。
- APP 之间通过异步 API 交互；跨模块事件由 `Application` 转投递。

### 单例、测试与接口生命周期

- `Board::GetInstance()` 只用于组合根；业务模块不应内部全局查找。
- 主机侧测试优先构造 `Application` 并注入假 `Board`/services，而不是依赖单例。
- devices 接口的 protected 非虚析构适合静态对象；若测试需要 mock 或 `unique_ptr`，
  改用虚析构或显式 deleter，并在平台契约中记录该选择。
- 为可测试性，`devices`/`services` 接口不应依赖 RTOS/厂商头；测试构建用普通 CMake
  编译纯逻辑 + mock 实现，HIL 再跑真实 BSP。

## 7. 中断 → 任务链路与回调上下文

```text
硬件 ISR / 厂商事件
        │  只做三件事：清标志、搬数据、投递
        ▼
port::QueueSendFromIsr / port::SemaphoreGiveFromIsr / port::TaskNotifyGiveFromIsr
        │  记录 woken
        ▼
port::YieldFromIsr(woken)
        ▼
APP 任务：QueueReceive / SemaphoreTake（阻塞等待）
        ▼
业务处理
```

| 上下文 | 能否阻塞 | 可调用 API | 该做什么 |
| --- | --- | --- | --- |
| 硬件 ISR | 否 | `...FromIsr` | 清标志、搬数据、投递、yield |
| 厂商事件任务（WiFi/MQTT/网络栈） | 不能久阻塞 | 非 FromISR | 只投递到 APP 队列 |
| 厂商事件回调（可能是 ISR） | 按 SDK 文档 | 按上下文 | 只记录状态/投递，不 stop/free |
| `esp_timer` / FreeRTOS timer | 不能阻塞 | 只投递 | 重活交给任务 |
| APP 任务 | 可以 | 全部 | 真正处理 |

补充规则：

- 命名约定：任何可能在 ISR 里调用的投递函数必须带 `FromIsr` 后缀；
  任务上下文用不带后缀的版本。不要用 `xPortIsInsideInterrupt()` 合并两者。
- **厂商事件回调里不要 stop/free**。例如配网、协议栈、总线控制器的
  `stop/close/deinit` 可能阻塞或与事件任务竞争；把动作投递到任务，
  或用一次性 timer 延迟到任务上下文执行。
- 中断优先级/中断级别是平台语义：Cortex-M 的 NVIC 规则不能直接套到
  ESP32/RISC-V。按平台契约与 `references/` 确认，不要凭经验写。
- 需要通知任务的中断，其优先级必须满足目标 RTOS 的 ISR 安全约束。

## 8. 任务、队列、同步原语

### 任务设计

- **一个并发域/资源 owner 一个任务**，不是“一个 APP 类一个任务”。
  同一队列/周期/生命周期的模块应合并；低频轮询优先共享软件定时器或主循环。
- 不要按外设拆成 `uart1_task`、`gpio_task`；按业务 owner 拆。
- 在 `references/module-matrix.md` 中记录任务预算（任务数、栈、优先级、周期、阻塞点）。
- 优先级表达实时性差异，不是重要性：

| 任务 | 优先级 | 说明 |
| --- | --- | --- |
| 硬实时控制 | 5 | 周期 + delay-until |
| main（MainEventLoop） | 4 | 只派发，快速返回 |
| network / 协议 | 3 | 阻塞在队列 |
| 采集 / storage | 2 | 周期或队列 |
| display / LED | 1 | 慢速、可被抢占 |
| Idle | 0 | 系统 |

- 高优先级任务必须会阻塞，否则低优先级任务饿死。
- 周期任务用 `port::DelayUntil` / `vTaskDelayUntil` 的等价物，不要用
  `Delay` 累积漂移；不要忙等，不要在任务里用厂商 `HAL_Delay`。
- 小 RAM 平台先做任务预算：减少任务数、共享 timer/主循环，再谈优先级。
- **栈大小**：
  - 单位以平台契约为准；`port::TaskOptions.stack_bytes` 统一用**字节**，
    由平台实现换算（ESP-IDF 直接用，原生 FreeRTOS 除以 `sizeof(StackType_t)`）。
  - 起点：纯逻辑 2KB，带日志/浮点 4~6KB；用高水位实测后留 1.5~2 倍余量。
  - 日志、`snprintf`、浮点格式化都很吃栈；不要用“感觉”定栈。

### 同步原语选择

| 需求 | 用什么 | 注意 |
| --- | --- | --- |
| 任务间传数据 | 队列 | 值拷贝；大块传索引 |
| APP 对外能力 | 异步 API + 自己的队列 | 默认做法 |
| 通知“有事件” | 二值信号量 / 任务通知 | 一对一优先任务通知 |
| ISR 通知任务 | `...FromIsr` 版本 | 必须处理 woken/yield |
| 保护共享外设（I2C/SPI） | 互斥量 | 有优先级继承；ISR 不能用 |
| 计数资源 | 计数信号量 | 初值 = 可用数量 |
| 多条件同时满足 | 事件组 | 位语义 |
| 字节流 | Stream Buffer | 与队列分工 |

互斥量边界：

- 只在任务里 take，give 必须成对；
- 不要持有互斥量做阻塞操作（等队列、写 Flash、网络发送）；
- 嵌套获取用递归互斥量。

临界区：

- 只用于极短、不与 RTOS API 交叉的代码；
- 单核 `taskENTER_CRITICAL` 只关当前核中断；双核共享数据必须用
  spinlock/原子操作或平台提供的跨核原语；
- 不要跨函数、跨阻塞调用持有。

共享总线：

- I2C/SPI 多设备时，确认驱动是否已有总线锁；没有就加互斥量；
- 事务必须有超时；不要在 ISR 里发起总线事务；
- 总线异常要有恢复策略（bus clear/reinit），并在启动时 probe 器件。

## 9. 定时器

选择顺序：

1. **默认用周期任务 + delay-until**：逻辑独立、可阻塞、可单独调优先级。
2. 只有在**低频（≤ 50 Hz）、实例多、每次工作量极小、回调只投递**时，才用共享软件
   定时器，避免每个实例一个任务栈。判断方法、成本对比和按键采样案例见
   `references/timer-vs-task.md`。

- APP/BSP 不直接调用 `xTimerCreate`；统一走 `port::CreateTimer/Start/Stop/Reset/DeleteTimer`。
- 软件定时器回调只投递，不阻塞、不等队列、不写 Flash、不 stop/free。
- 区分 FreeRTOS software timer 与厂商 timer：
  - FreeRTOS timer 回调运行在定时器任务；
  - `esp_timer` 可能是任务或 ISR（取决于 dispatch 方式）；
  - 厂商协议栈 timer 的上下文按 SDK 文档确认。
- 需要释放资源/关闭外设时，把动作投递到普通任务执行。

## 10. 分配策略

| 场景 | 策略 | 注意 |
| --- | --- | --- |
| RAM 小、无 PSRAM、长期运行 | 静态创建 | 占用在链接期可见 |
| RAM 较大（ESP32 等） | init 阶段动态创建 | 检查返回值，失败即停机 |
| PSRAM / DMA 缓冲 | 平台 memory capability | 按平台 API 指定 |
| 频繁创建/销毁 | 预分配池 + 位图 | 与平台无关 |

> 本 skill 的 `port` 模板只覆盖动态创建。静态分配必须在平台 port 中按 SDK 语义
> 扩展 `xTaskCreateStatic` / `xQueueCreateStatic` / `xTimerCreateStatic`，并在
> 平台契约中记录缓冲区类型、对齐要求和栈单位（word 还是 byte）。

无论哪种策略：

1. 不在 ISR 里分配；
2. 运行期不反复 create/delete；
3. 分配失败必须可见（assert/abort 或明确的降级），禁止静默使用空句柄。

C++ 侧注意：

- 若工程关闭异常/RTTI（常见于 MCU）：`new` 失败可能直接 abort，
  需要可恢复时用 `new (std::nothrow)` 并检查。
- 函数内 `static` 的首次构造有 `__cxa_guard`；不要在 ISR 里首次构造。
- `volatile` 不是原子操作；不要写 `++volatile_var`（C++20 已废弃）。
  计数用 `std::atomic`、临界区或任务内单写者。
- 大字体表/常量表用 `constexpr` 放 flash，不要放 RAM。

### 堆 / STL / `std::function` 策略

| 平台 | 允许 | 限制 |
| --- | --- | --- |
| ESP32 / RAM 充足 | `std::string`、`std::map`、`std::function`（任务上下文） | 不在 ISR/高频路径使用；检查分配失败；热路径用固定缓冲/池 |
| STM32 / RAM 紧张 | 固定数组、`etl` 容器、函数指针 + `void*` context | 避免捕获式 `std::function`、频繁 `std::string`、运行期容器增删 |
| 所有平台 | 静态表、`constexpr` 常量 | 不在 ISR 分配；不在回调里做大块 JSON/字符串拼接 |

`std::function` 可能因捕获而分配；`std::string`/`std::map` 也可能分配。
如果平台契约要求“无运行期 malloc”，这些接口必须替换为函数指针 + context
或固定容量容器，并在模块矩阵中记录。

## 11. 失败、降级与可观测性

- 外设 init 失败：按产品策略选择 fail-fast 或降级；降级时 `ready_` 标志 +
  安全返回，不能解引用空句柄。
- 重试/超时/退避属于 APP 或 BSP 的策略，不要泄漏给无关模块。
- 有效性语义：
  - 采样无效（断线、超量程）要显式标记，不要用 0 冒充有效值；
  - 显示/上云要区分“无效”和“陈旧”；明确是否使用 last-known-good。
- 计数器：队列丢弃、发布失败、重连次数、传感器错误；周期日志暴露。
- 诊断：栈高水位、heap、任务状态、看门狗；调试期开
  `configCHECK_FOR_STACK_OVERFLOW`，用高水位定量。
- 日志：TAG + 级别；高频日志限流；**不要打印密码、token、密钥**。
- 启动自检：I2C scan/probe、器件 ID、总线电压范围、屏幕方向。

## 12. 时间与连接状态机

### 时间

- 超时/周期用 tick 或单调时钟，不用墙钟；
- 墙钟只用于业务时间、日志时间戳、token 过期；
- RTC 未同步时 `time()` 不可信：先 SNTP/NTP，或使用固定/可配置过期时间；
- 注意 `TickType_t` 溢出；长时间运行用 64 位单调时间做诊断。

### 连接状态机

WiFi / MQTT / 配网 / 重连必须有**唯一 owner**（一个 APP 或一个 BSP 模块），
不要让多个任务同时调用 connect/stop。

```text
INIT → PROVISIONING → WIFI_CONNECTED → MQTT_CONNECTING → ONLINE
  │          │                │                │
  └──────────┴────────────────┴────────────────┴→ RETRY（退避）
```

- `Start()` / `Connect()` 必须幂等；
- 配网/重连标志要在启动动作**之前**置位，避免事件任务与初始化路径重复启动；
- 断线重连有退避与上限；重新配网要有明确入口；
- 离线数据策略：丢弃最新 / 保留最新 / 环形缓冲 / Flash 队列；
  队列满时计数，不要无限增长；
- 鉴权失败（token/密钥）与网络失败分开处理。

## 13. 代码风格

- `app` / `bsp` / `devices`：类名 PascalCase、方法 PascalCase、成员后缀 `_`；
  文件名 snake_case；缩进用 Tab；K&R 花括号；`#pragma once`。
- 注释中文，只写必要信息；不写“调用了函数”这类废话。
- 回调统一使用无捕获 lambda / 静态成员函数；
  任务入口用静态成员函数，参数 `void *ctx`。
- 句柄命名：`xxx_queue_` / `xxx_task_` / `xxx_sem_` / `xxx_mutex_`。
- 头文件只暴露必要接口；`devices/` 不 include RTOS/厂商头。

## 14. 反模式清单

| 反模式 | 正确做法 |
| --- | --- |
| `Application::Start()` 里调用 `vTaskStartScheduler()` | 平台入口调用一次；ESP-IDF 为空实现 |
| 栈大小按 word 猜 | 先填平台契约，`port` API 用 `stack_bytes` |
| APP 公有 API 内部阻塞/等信号量 | 只投递事件后返回 |
| APP 公有 API 用无限等待入队 | timeout = 0，满则计数 |
| 构造函数里创建任务 | 在 `Start()` 路径创建 |
| APP 内部 `Board::GetInstance()` | 构造函数注入 |
| APP 之间同步互调 | 调对方异步 API |
| 所有事件塞进 MainEventLoop | 模块内部事件走各自队列 |
| MainEventLoop handler 做重活 | 只更新状态 + 投递 |
| ISR 里调用非 FromISR API / 打印 / 延时 | `...FromIsr` + yield |
| 厂商事件回调里 stop/free | 投递到任务，或一次性 timer 延迟 |
| 中断优先级凭经验写 | 按平台契约/参考确认 |
| 用 `vTaskDelay` 做周期 | delay-until |
| `while (!flag) {}` 忙等 | 阻塞在队列/信号量/通知 |
| 定时器回调里阻塞 | 只投递 |
| 每个低频轮询实例（按键/LED）都建一个任务 | 共享软件定时器（回调只投递） |
| 在 ISR/热路径使用捕获式 `std::function` 或频繁 `std::string` | 函数指针 + context、固定缓冲、池化 |
| 实现层头文件放在公共 `include/` 给 app 用 | 放 `src/` + `PRIV_INCLUDE_DIRS`，只暴露接口 |
| ISR 里拿互斥量 | 不可能；用队列/短临界区 |
| `devices/` 里 include RTOS/厂商头 | 保持纯抽象 |
| 运行期反复 create/delete/new | init 阶段创建一次 |
| `++volatile` 当原子计数 | atomic / 临界区 / 单写者 |
| 不处理初始化失败 | `ready_` + 明确降级/停机 |
| 离线数据无限缓存 | 明确丢弃/保留最新/环形缓冲 |
| 日志打印密码/token | 脱敏或只打印长度/哈希 |
| app 同时无规则依赖 bsp 和 devices | 选定 Layered/DIP/Hybrid 并用构建系统强制 |
| Board.hpp 暴露具体驱动类型 | 暴露 devices/services 接口或 BoardServices |

## 15. 检查清单

- [ ] 架构模式已选定（Layered / DIP / Hybrid），选择理由与模块边界已记录
- [ ] 构建系统已强制依赖方向；临时 include 不允许的层会编译失败
- [ ] DIP 模式下 app 不 include `bsp`/`Board.hpp`，Board 只暴露接口或 `BoardServices`
- [ ] Hybrid 模式下逐模块列出依赖路径，没有同一模块双路径
- [ ] 实现层没有公共头文件，或公共头只包含接口类型；BSP 实现头放 `src/` + `PRIV_INCLUDE_DIRS`
- [ ] `references/module-matrix.md` 已填写模块路径、任务预算、分配策略
- [ ] 平台契约已填写并记录到 `docs/platform.md`
- [ ] 入口函数与调度器启动模型正确；没有重复启动调度器
- [ ] 栈单位确认；`port` API 用 `stack_bytes`；栈大小经过高水位验证
- [ ] ISR 只清标志/搬数据/投递，使用 `...FromIsr` 并处理 woken
- [ ] 中断优先级/中断级别满足目标 RTOS 的 ISR 安全约束
- [ ] 厂商事件/定时器回调上下文已确认；回调里没有 stop/free/阻塞
- [ ] 低频轮询实例已评估“任务 vs 共享软件定时器”，没有为每个实例单独建栈
- [ ] `Application` 构造函数只建队列/注册回调，任务在 `Start()` 创建
- [ ] `MainEventLoop()` 只派发中断事件与跨模块事件，handler 快速返回
- [ ] 每个 APP/并发域自持队列与任务，公有 API 都是“投递后返回”
- [ ] APP 依赖全部构造注入，没有内部全局查找；单例只出现在组合根
- [ ] 队列满/发布失败/重连有计数，日志可观测
- [ ] 初始化失败有明确策略；降级路径不会解引用空句柄
- [ ] 时间基准确认（tick vs 墙钟），token/证书过期策略明确
- [ ] 连接/配网/重连有唯一 owner，状态迁移可读，操作幂等
- [ ] 离线数据策略明确，缓存有上限
- [ ] I2C/SPI 事务有超时、总线锁/恢复策略；启动时 probe
- [ ] 没有运行期 `new`/`malloc`/STL 容器反复增删；`std::function`/`std::string` 的分配风险已评估
- [ ] `port` 覆盖 timer/栈高水位/heap 诊断；静态分配策略已记录
- [ ] 没有日志泄漏密码/token/密钥；`sdkconfig` 等敏感配置不入库
- [ ] 产品级检查已过：OTA/分区、低功耗、看门狗、NVS 迁移、安全、离线缓存（见 `references/product-architecture.md`）
- [ ] 有主机侧测试入口或 mock 方案；关键状态机有 HIL/台架测试清单
- [ ] 无 `sdkconfig` 的全新构建、所有条件编译分支分别构建通过
- [ ] `size`/分区余量、栈水位、启动日志 smoke test 通过

## 16. 平台参考与模板索引

| 文件 | 用途 |
| --- | --- |
| `references/platform-profile.md` | 平台契约模板（含架构模式选择）、逐项查证方法、示例 profile |
| `references/freertos-port-semantics.md` | 原生 FreeRTOS / ESP-IDF / CMSIS-RTOS2 语义差异与映射 |
| `references/esp-idf.md` | ESP-IDF 启动、组件、IDF 6 API 迁移、回调上下文、验证 |
| `references/stm32-cubemx.md` | STM32/CubeMX、HAL 时基、NVIC、静态分配、EIDE/Keil |
| `references/callback-contexts.md` | ISR / 厂商事件 / 定时器 / 任务上下文矩阵与案例 |
| `references/timer-vs-task.md` | 周期任务与软件定时器的选择依据、成本对比、按键采样案例 |
| `references/module-matrix.md` | Hybrid/DIP 逐模块依赖、任务预算、分配策略模板 |
| `references/product-architecture.md` | OTA/低功耗/看门狗/NVS 迁移/安全/离线缓存检查清单 |
| `templates/port/rtos_port.hpp` | 统一 port 接口（任务/队列/信号量/定时器/临界区/时间/诊断） |
| `templates/esp-idf/` | ESP-IDF port 实现、`app_main`、组件 CMake、Kconfig、分区 |
| `templates/stm32/` | 原生 FreeRTOS port 实现、栈 word→byte 换算、FreeRTOSConfig 说明 |
| `templates/host-test/` | 主机侧测试与 mock 目录骨架 |
| `checklists/bring-up.md` | 硬件与固件 bring-up 检查清单 |
| `examples/sensor-to-cloud/README.md` | 采集→滤波→显示→MQTT 的完整案例映射 |

**记住**：核心规则是平台无关的；平台语义必须通过契约确认，不要凭记忆或旧版本经验生成代码。
