---
name: mcu-rtos-task-loop
description: '编写或改造基于 RTOS 的 MCU 工程（FreeRTOS / CMSIS-RTOS2，Cortex-M / STM32 / ESP32 / 嵌入式 C++）时使用本技能：中断只做三件事并全部走 FromISR API、Application 用 MainEventLoop 统一派发跨模块事件、每个 APP 类自己持有队列与任务并以异步 API 暴露能力（display->DrawPicture() 只投递事件）、依赖在构造时由 Board 注入、任务/队列只在初始化阶段创建一次（小 RAM 平台静态分配，ESP32 等大 RAM 平台可用动态分配）、HAL 时基改由 TIM 提供、SysTick/PendSV/SVC 交给 FreeRTOS port。适用于新建 RTOS 工程、给现有工程加 RTOS、新增 APP 模块/任务/队列/软件定时器/串口协议，以及配置 EIDE 与 Keil 工程（FreeRTOS 文件清单、include 路径、中断优先级）。Use when adding tasks, queues, semaphores, software timers, ISR-to-task paths, or app modules with async APIs in a FreeRTOS/CMSIS-RTOS2 project, and generate code that follows these patterns instead of inventing new ones.'
argument-hint: '要新增的 APP 模块 / 任务 / 数据流，例如"加一个网络模块，带异步连接与发送"'
---

# RTOS 多任务工程模式（FreeRTOS / CMSIS-RTOS2）

本技能约束"**中断 → 队列 → 任务**"这条链路，并规定 APP 层的组织方式：

- 中断只把数据/事件投递给队列；
- `Application` 用 **`MainEventLoop()`** 派发跨模块事件与中断事件；
- **每个 APP 类自己持有队列和任务**，对外只暴露**异步 API**（投递事件后立即返回）；
- APP 对象的依赖由 `Application` 在构造时注入（`Board` 提供板级资源），APP 内部不自己做全局查找。

默认按 **FreeRTOS 原生 API + 静态分配**写代码；用 CMSIS-RTOS2 的工程看文末对照表。

## 何时使用

- 工程里有 FreeRTOS（`tasks.c` / `queue.c` / `port.c`）或 CMSIS-RTOS2（`cmsis_os2.h`）
- 要新增 APP 模块、任务、队列、信号量、事件组、软件定时器、任务通知
- 要新增外设、协议解析、周期性采集
- 要把现有工程加上 RTOS 并划分任务
- 要配置 EIDE / Keil 的 FreeRTOS 文件清单与中断优先级

## 工程骨架

```
<project>/
├── app/            # Application（MainEventLoop）+ 各 APP 类（Network / Display / Storage…）
├── bsp/            # Board 单例、驱动实现、HAL 回调 → 投递
├── devices/        # 器件抽象：不 include 厂商头，也不 include FreeRTOS.h
├── port/           # mcu_cmsis.h：唯一的 MCU 依赖点
├── Core/           # 厂商生成：main.c、*_it.c、FreeRTOSConfig.h
├── Drivers/        # CMSIS + HAL
├── Middlewares/Third_Party/FreeRTOS/   # 内核 + port（静态分配下不需要 heap）
├── MDK-ARM/        # Keil 工程（AC6 工程才有）
└── EIDE/           # EIDE 工程
```

分层与依赖方向：

```
app → bsp → devices → utility → port → CMSIS
 │      │
 └──────┴──→ FreeRTOS（tasks / queue / timers）
```

- `app/`、`bsp/` 可以 include `FreeRTOS.h` / `task.h` / `queue.h`；
- `devices/` **不 include FreeRTOS**，也不调用 `vTaskDelay` / 信号量——阻塞、超时、重试属于 APP 的策略，不属于器件语义；
- `port/` 只提供 CMSIS 桥接。

## 三条铁律

1. **中断只做三件事，并且只用 FromISR API**：清标志、搬数据、`xQueueSendFromISR` / `xSemaphoreGiveFromISR` / `xTaskNotifyFromISR`，最后处理 `xHigherPriorityTaskWoken`。禁止在 ISR 里 `printf`、`HAL_Delay`、阻塞发送、调用非 FromISR 的 RTOS API。
2. **资源只在初始化阶段创建一次**：任务/队列/信号量/定时器在 `Start()` 路径上创建，运行期不 create/delete，ISR 里不分配。
   - 小 RAM 平台（< 64 KB、无 PSRAM、7×24 长跑）：静态创建（`xTaskCreateStatic` / `xQueueCreateStatic`），`configSUPPORT_DYNAMIC_ALLOCATION = 0`；
   - 大 RAM 平台（ESP32 等）：可在 init 阶段用 `xTaskCreate` / `xQueueCreate` / `heap_caps_malloc` 动态创建，必须检查返回值，失败即停机；
   - 两种情况都禁止：运行期反复 `new` / `malloc`、在热路径用 STL 容器增删。详见「分配策略：静态还是动态」。
3. **共享数据用 RTOS 原语**：任务之间用队列传递数据、用互斥量保护资源；只有"极短临界区且双方同核"才用 `taskENTER_CRITICAL`。**ISR 不能拿互斥量**。

## Application：MainEventLoop 与任务创建时机

`Application` 是单例，持有所有 APP 对象和**主事件队列**。它的职责只有三件：

1. 构造函数：创建主队列、注册 BSP 回调（回调只 `PostXxx`）；
2. `Start()`：创建主任务并启动调度器；
3. `MainEventLoop()`：派发事件；`HandleStart()` 里启动各 APP 对象（由它们各自建任务）。

```cpp
// app/application.hpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "display.hpp"
#include "network.hpp"

// 应用层入口（单例）：持有 APP 对象与主事件队列。
// MainEventLoop() 是全工程唯一接收"中断事件 / 跨模块事件"的派发点。
class Application {
public:
	static Application &GetInstance() {
		static Application instance;
		return instance;
	}

	Application(const Application &) = delete;
	Application &operator=(const Application &) = delete;

	// 创建主任务并启动调度器；正常情况下不返回
	void Start();

	// 供 BSP / 驱动在中断里调用：只投递，不解析
	void PostKeyReleasedEventFromISR(uint8_t index);
	// 供 APP 对象在任务上下文调用
	void PostNetworkConnectedEvent();

private:
	static constexpr size_t MAIN_QUEUE_LENGTH = 16;
	static constexpr uint32_t MAIN_TASK_STACK_WORDS = 512;   // 单位：word
	static constexpr UBaseType_t MAIN_TASK_PRIORITY = 4;

	static constexpr uint32_t START_EVENT = 0x00000001u;
	static constexpr uint32_t KEY_RELEASED_EVENT = 0x00000002u;
	static constexpr uint32_t NETWORK_CONNECTED_EVENT = 0x00000003u;

	struct QueueItem {
		uint32_t event;
		union {
			struct {
				uint8_t index;
			} key_released;
		} data;
	};

	Application();

	static void MainTaskEntry(void *ctx);
	void MainEventLoop();

	void PostEvent(uint32_t event);
	bool PostFromISR(const QueueItem &item);

	void HandleStart();
	void HandleKeyReleased(const QueueItem &item);
	void HandleNetworkConnected();

	// 主队列 / 主任务（静态）
	StaticQueue_t main_queue_buffer_ = {};
	alignas(8) uint8_t main_queue_storage_[MAIN_QUEUE_LENGTH * sizeof(QueueItem)] = {};
	QueueHandle_t main_queue_ = nullptr;

	StaticTask_t main_task_buffer_ = {};
	StackType_t main_task_stack_[MAIN_TASK_STACK_WORDS] = {};

	// APP 对象：依赖在这里注入，构造时只保存引用
	Network network_;
	Display display_;
};
```

```cpp
// app/application.cpp
#include "application.hpp"

#include "board.hpp"

Application::Application()
	: network_(Board::GetInstance().Get4GModule()),
	  display_(Board::GetInstance().GetDisplay()) {
	main_queue_ = xQueueCreateStatic(MAIN_QUEUE_LENGTH, sizeof(QueueItem), main_queue_storage_, &main_queue_buffer_);

	// 中断侧只投递；回调里不解析、不打印
	Board::GetInstance().GetButton(Board::BUTTON0_INDEX)->SetReleasedCallback(
		+[](void *ctx) {
			static_cast<Application *>(ctx)->PostKeyReleasedEventFromISR(
				static_cast<uint8_t>(Board::BUTTON0_INDEX));
		},
		this);
}

void Application::Start() {
	TaskHandle_t main_task = xTaskCreateStatic(&Application::MainTaskEntry, "main",
		MAIN_TASK_STACK_WORDS, this, MAIN_TASK_PRIORITY, main_task_stack_, &main_task_buffer_);
	configASSERT(main_task != nullptr);

	vTaskStartScheduler();   // 返回说明任务创建失败 / 内存不足
	for (;;) {
	}
}

void Application::MainTaskEntry(void *ctx) {
	static_cast<Application *>(ctx)->MainEventLoop();
}

void Application::MainEventLoop() {
	PostEvent(START_EVENT);   // 初始化放在任务上下文、调度器启动之后

	QueueItem item = {};
	for (;;) {
		if (xQueueReceive(main_queue_, &item, portMAX_DELAY) != pdPASS) {
			continue;
		}

		switch (item.event) {
		case START_EVENT:
			HandleStart();
			break;
		case KEY_RELEASED_EVENT:
			HandleKeyReleased(item);
			break;
		case NETWORK_CONNECTED_EVENT:
			HandleNetworkConnected();
			break;
		default:
			break;
		}
	}
}

void Application::HandleStart() {
	// APP 对象在这里启动：构造只保存依赖，Start() 才创建自己的任务
	network_.Start();
	display_.Start();

	display_.ShowText("boot ok");   // 异步 API：投递后立即返回
	network_.Connect();
}

void Application::HandleKeyReleased(const QueueItem &item) {
	if (item.data.key_released.index != Board::BUTTON0_INDEX) {
		return;
	}
	display_.ShowText("Button0 released");
}
```

要点：

- **构造不建任务**：`Application` 构造函数只建队列、注册回调；任务统一在 `HandleStart()` 里创建，此时调度器已运行、所有单例构造完毕。
- `MainEventLoop()` 只处理"中断事件"和"跨模块事件"；模块内部的事件走各自队列，不要全塞进主队列，否则主循环成为瓶颈。
- 主循环里的 handler 必须快速返回：只做状态更新和**投递**，重活交给对应 APP 任务。
- 队列必须在开中断前创建；`vTaskStartScheduler()` 返回要有兜底。
- 单例的首次 `GetInstance()` 必须发生在**调度器启动之前**（`main()` 里初始化阶段），不要等到任务或中断里才第一次构造——函数内 `static` 带 `__cxa_guard`，并发首次构造会多一次原子判断，且中断里首次构造等于在 ISR 里做初始化。

## APP 类：构造注入依赖 + 异步 API

每个 APP 类是一个**自治单元**：自己持有队列与任务，对外只暴露"投递事件"的方法。

> 下例按**静态分配策略**写（小 RAM 平台）。换成 ESP32 这类平台时，只需把 `Start()` 里的 `xQueueCreateStatic` / `xTaskCreateStatic` 换成 `xQueueCreate` / `xTaskCreatePinnedToCore`、删掉静态成员，其余结构与 API 完全不变。

### 构造：依赖从外面注入

```cpp
// app/network.hpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "network_module.hpp"   // devices/ 的抽象，见「devices / bsp 契约」

// APP 对象：拥有自己的队列与任务。
// 对外 API 全部是"投递事件后立即返回"，真正的工作在 TaskLoop() 里做。
class Network {
public:
	explicit Network(NetworkModule &module)
		: module_(module) {}

	void Start();                                   // 创建队列与任务（在 HandleStart() 中调用）
	void Connect();                                 // 异步：投递 CONNECT 事件
	void Send(const void *data, uint16_t size);     // 异步：写入缓冲池，队列里只传槽位号
	void SetConnectedCallback(void (*cb)(void *), void *ctx);

private:
	static constexpr size_t QUEUE_LENGTH = 8;
	static constexpr size_t TX_SLOT_COUNT = 4;      // 发送缓冲池槽位数
	static constexpr size_t TX_SLOT_SIZE = 128;     // 单槽字节数
	static constexpr uint32_t TASK_STACK_WORDS = 512;
	static constexpr UBaseType_t TASK_PRIORITY = 3;

	enum : uint32_t {
		CONNECT_EVENT = 0x00000001u,
		SEND_EVENT = 0x00000002u,
		RECEIVED_EVENT = 0x00000003u,
	};

	// 队列元素保持很小：大块数据放缓冲池，索引通过槽位号传递
	struct Item {
		uint32_t event;
		union {
			struct {
				uint8_t slot;       // tx_pool_ 下标
				uint16_t size;
			} send;
			struct {
				uint16_t size;
			} received;
		} data;
	};

	static void TaskEntry(void *ctx);
	void TaskLoop();
	bool Post(const Item &item);            // 任务上下文调用
	bool PostFromISR(const Item &item);     // ISR 调用
	void HandleConnect();
	void HandleSend(const Item &item);
	void HandleReceived(const Item &item);

	// 发送缓冲池：固定槽位 + 位图，无堆、无碎片
	int AcquireSlot();
	void ReleaseSlot(uint8_t slot);

	NetworkModule &module_;                 // 注入的依赖，生命周期由 Board 保证

	StaticQueue_t queue_buffer_ = {};
	alignas(8) uint8_t queue_storage_[QUEUE_LENGTH * sizeof(Item)] = {};
	QueueHandle_t queue_ = nullptr;

	StaticTask_t task_buffer_ = {};
	StackType_t task_stack_[TASK_STACK_WORDS] = {};

	alignas(8) uint8_t tx_pool_[TX_SLOT_COUNT][TX_SLOT_SIZE] = {};
	volatile uint32_t tx_slot_used_ = 0;    // 位图：bit n = 槽位 n 占用

	void (*connected_cb_)(void *) = nullptr;
	void *connected_ctx_ = nullptr;

	uint32_t dropped_ = 0;
};
```

```cpp
// app/network.cpp
#include <string.h>

#include "network.hpp"

void Network::Start() {
	if (queue_ != nullptr) {
		return;   // 幂等：重复调用不重建
	}
	queue_ = xQueueCreateStatic(QUEUE_LENGTH, sizeof(Item), queue_storage_, &queue_buffer_);
	configASSERT(queue_ != nullptr);

	TaskHandle_t task = xTaskCreateStatic(&Network::TaskEntry, "network",
		TASK_STACK_WORDS, this, TASK_PRIORITY, task_stack_, &task_buffer_);
	configASSERT(task != nullptr);
}

// 公有 API：只校验、拷贝、投递，然后立刻返回
void Network::Connect() {
	Item item = {};
	item.event = CONNECT_EVENT;
	(void)Post(item);
}

void Network::Send(const void *data, uint16_t size) {
	if (data == nullptr || size == 0 || size > TX_SLOT_SIZE) {
		return;
	}

	const int slot = AcquireSlot();
	if (slot < 0) {                 // 池满：丢弃并计数，绝不阻塞
		++dropped_;
		return;
	}
	memcpy(tx_pool_[slot], data, size);

	Item item = {};
	item.event = SEND_EVENT;
	item.data.send.slot = static_cast<uint8_t>(slot);
	item.data.send.size = size;

	if (!Post(item)) {              // 队列满：把槽位还回去，避免泄漏
		ReleaseSlot(static_cast<uint8_t>(slot));
	}
}

int Network::AcquireSlot() {
	int slot = -1;
	taskENTER_CRITICAL();
	for (size_t i = 0; i < TX_SLOT_COUNT; ++i) {
		if ((tx_slot_used_ & (1UL << i)) == 0) {
			tx_slot_used_ |= (1UL << i);
			slot = static_cast<int>(i);
			break;
		}
	}
	taskEXIT_CRITICAL();
	return slot;
}

void Network::ReleaseSlot(uint8_t slot) {
	taskENTER_CRITICAL();
	tx_slot_used_ &= ~(1UL << slot);
	taskEXIT_CRITICAL();
}

bool Network::Post(const Item &item) {
	if (queue_ == nullptr) {        // Start() 之前调用：直接丢弃，不要解引用空句柄
		return false;
	}
	if (xQueueSend(queue_, &item, 0) != pdPASS) {   // 0：绝不阻塞调用者
		++dropped_;
		return false;
	}
	return true;
}

bool Network::PostFromISR(const Item &item) {
	if (queue_ == nullptr) {
		return false;
	}
	BaseType_t woken = pdFALSE;
	const BaseType_t ok = xQueueSendFromISR(queue_, &item, &woken);
	portYIELD_FROM_ISR(woken);
	if (ok != pdPASS) {
		++dropped_;
		return false;
	}
	return true;
}

void Network::TaskEntry(void *ctx) {
	static_cast<Network *>(ctx)->TaskLoop();
}

void Network::TaskLoop() {
	Item item = {};
	for (;;) {
		if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdPASS) {
			continue;
		}

		switch (item.event) {
		case CONNECT_EVENT:
			HandleConnect();
			break;
		case SEND_EVENT:
			HandleSend(item);
			break;
		case RECEIVED_EVENT:
			HandleReceived(item);
			break;
		default:
			break;
		}
	}
}

void Network::HandleConnect() {
	if (module_.Connect() && connected_cb_ != nullptr) {
		connected_cb_(connected_ctx_);
	}
}

void Network::HandleReceived(const Item &item) {
	(void)item;
	// 解析收包后，需要通知其它模块时向 Application 投递事件（异步）
}

void Network::HandleSend(const Item &item) {
	const uint8_t slot = item.data.send.slot;
	// 阻塞式收发只允许出现在任务上下文
	module_.Send(tx_pool_[slot], item.data.send.size);
	ReleaseSlot(slot);              // 发送完成才归还槽位
}

void Network::SetConnectedCallback(void (*cb)(void *), void *ctx) {
	connected_cb_ = cb;
	connected_ctx_ = ctx;
}
```

### 另一个例子：`Display::DrawPicture()`

同样的形状，注意**大图不进队列**——只传图片 ID，像素由任务按 ID 去 Flash / 外部存储里取：

```cpp
// app/display.hpp（节选）
class Display {
public:
	explicit Display(DisplayPanel &panel)
		: panel_(panel) {}

	void Start();
	void DrawPicture(uint16_t picture_id);   // 异步：只投递 ID
	void ShowText(const char *text);         // 异步：短文本直接拷贝

private:
	static constexpr size_t MAX_TEXT_LENGTH = 32;

	enum : uint32_t {
		DRAW_PICTURE_EVENT = 0x00000001u,
		SHOW_TEXT_EVENT = 0x00000002u,
	};

	struct Item {
		uint32_t event;
		union {
			struct {
				uint16_t picture_id;
			} draw_picture;
			struct {
				char text[MAX_TEXT_LENGTH];
			} show_text;
		} data;
	};

	// 队列 / 任务成员与 Network 相同：queue_buffer_、queue_storage_、queue_、
	// task_buffer_、task_stack_，以及 Post() / PostFromISR() / TaskLoop()

	void TaskLoop();

	void HandleDrawPicture(const Item &item) {
		panel_.Draw(item.data.draw_picture.picture_id);   // 阻塞式绘制，跑在本任务里
	}
};
```

```cpp
void Display::DrawPicture(uint16_t picture_id) {
	Item item = {};
	item.event = DRAW_PICTURE_EVENT;
	item.data.draw_picture.picture_id = picture_id;
	(void)Post(item);            // 投递后立刻返回，调用者不等待绘制完成
}
```

> `Post()` 里要先判断 `queue_ != nullptr`：`Start()` 之前调用公有 API 很常见（例如某个模块在 `HandleStart()` 里先于对象启动就被调用），此时直接返回 `false`，不要让 `xQueueSend(nullptr, ...)` 触发内核 assert。

调用方（任意 APP 任务）：

```cpp
display_.DrawPicture(7);          // 只是一次入队，可能不满 1 µs
```

### 异步 API 的三条规则

1. **公有 API 只做三件事**：参数校验、拷贝、`xQueueSend(..., 0)`，然后返回。
   `0` 是必须的——用 `portMAX_DELAY` 会让调用者（可能是主循环任务）被队列反压阻塞。
2. **返回值表示"是否入队"，不表示"是否完成"**。要结果就注册回调（`SetConnectedCallback`）、用事件组，或由对象再向 `Application` 投递事件。
3. **队列满时计数，不静默**：`++dropped_`，并在调试期用 `configASSERT` 或周期日志暴露出来。

### 跨模块调用也走异步

```cpp
void Application::HandleNetworkConnected() {
	// 不是让 network 直接画，而是让 display 在自己的任务里画
	display_.ShowText("online");
}
```

`display_.ShowText()` 与 `network_.Send()` 都是"投递后返回"，所以**任何 APP 任务都可以安全调用任何 APP 的公有 API**，不需要考虑对方当前在做什么，也不会形成锁依赖。

### 什么时候不需要异步

- 纯查询、无副作用、返回 POD：`bool IsConnected() const` 可以直接读 `volatile` 状态位；
- 但**不要**在公有 API 里 `xQueueReceive`、等信号量、调 `module_.Send()` 阻塞——那会把对象的内部并发模型泄漏给调用者。

## 中断 → 任务的链路

```
HAL_UART_RxCpltCallback / EXTI / TIM ISR
        │  只做三件事：清标志、搬数据、FromISR 投递
        ▼
PostXxxFromISR() → xQueueSendFromISR / xTaskNotifyFromISR
        │  记住 woken
        ▼
portYIELD_FROM_ISR(woken)
        ▼
任务：xQueueReceive / ulTaskNotifyTake（portMAX_DELAY 阻塞）
        ▼
业务处理
```

`Board` 仍独占 HAL 弱回调，只把"有数据"转给驱动，驱动再调上层注册的回调：

```cpp
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance != USART1) {
		return;
	}
	Board::GetInstance().GetSerial()->HandleRxCpltEvent();   // 只上报，不解析
}
```

命名约定：**任何可能在 ISR 里被调用的投递函数都必须带 `FromISR` 后缀**，实现里只用 `...FromISR` API。任务上下文用不带后缀的版本。不要合并成一个函数内部判断 `xPortIsInsideInterrupt()`——可读性差且容易漏 `portYIELD_FROM_ISR`。

### 中断优先级（最容易出问题的一条）

调用任何 `...FromISR` 的中断，其 **NVIC 抢占优先级数值必须 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`**（Cortex-M 上数值越大优先级越低）：

```c
#define configPRIO_BITS                              4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
```

```c
HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);   // 5..15 才能调 FromISR API
HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
```

- 优先级 0..4 的中断**不可以**调用任何 RTOS API（只能碰寄存器）；
- 优先级分组必须是全抢占位（`NVIC_PRIORITYGROUP_4`），`HAL_Init()` 默认就是；
- 调试期开 `configASSERT`，`vPortValidateInterruptPriority()` 会直接抓出违规。

## 分配策略：静态还是动态

"静态创建"不是 RTOS 的硬性要求，它是**小 RAM 平台上换取确定性的手段**。选哪种只看平台与生命周期，不看个人偏好。

| 平台 / 场景 | 策略 | 配置 |
|---|---|---|
| RAM < 64 KB、无 PSRAM、7×24 长时间运行 | 静态创建，内存占用链接期可见 | `configSUPPORT_STATIC_ALLOCATION = 1`、`configSUPPORT_DYNAMIC_ALLOCATION = 0` |
| RAM ≥ 256 KB（ESP32 等）、对象在 init 阶段创建后不再销毁 | init 阶段动态创建，运行期不释放 | 两者都开，`xTaskCreate` / `xQueueCreate` |
| 需要 PSRAM 或 DMA 缓冲 | 动态 + memory capability | `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`、`MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`，或 IDF 的 `xTaskCreateWithCaps` / `xQueueCreateWithCaps` |
| 频繁创建 / 销毁的对象 | 预分配池（静态数组 + 空闲位图/链表） | 与平台无关 |

### 为什么小 RAM 平台要静态

- 堆碎片会在运行数天后让分配失败，且失败点不可预测；
- 静态数组的占用在 `.map` 里可见，超 RAM 在链接期就报错；
- 不需要 `heap_4.c`，也不需要链接脚本里精确切出 heap 区。

### 为什么 ESP32 这类平台不必静态

- **静态数组进 `.bss`，默认落在 internal DRAM**，而 internal DRAM 是最紧张的（Wi-Fi / BT 协议栈占去一大块），大缓冲区静态化就用不上 PSRAM；
- **静态分配表达不了 memory capability**：DMA 缓冲必须 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`，只有 `heap_caps_*` 能在运行时指定；
- IDF 自身组件全部用 `xTaskCreate` / `xQueueCreate`，跟惯例走；
- 栈大小还能先用 `uxTaskGetStackHighWaterMark` 实测再定值（定完仍应固定，不要每次动态调整）。

ESP-IDF 的 `FreeRTOSConfig.h` 通常同时打开静态与动态分配（以你的 IDF 版本为准），不存在只能选一个的问题。

### 与 RAM 大小无关的三条约束

无论静态还是动态，这三条都成立：

1. **不在 ISR 里分配**——ISR 里任何 `malloc` 都是未定义行为；
2. **不在运行期反复 create/delete**——会抖动并产生碎片，用初始化时一次性创建或对象池；
3. **分配失败必须可见**——检查返回值，失败走 `configASSERT` / 停机，禁止静默使用 `nullptr`。

C++ 一侧同理：大 RAM 平台上 `new` / `std::string` / `std::function` 是**允许**的，但热路径仍不建议（堆抖动、代码体积）；这是风格选择，不是硬规则。

### 大 RAM 平台的动态创建写法

```cpp
// ESP32 / ESP-IDF：初始化阶段创建一次，运行期只使用
void Network::Start() {
	if (queue_ != nullptr) {
		return;
	}

	queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(Item));
	if (queue_ == nullptr) {
		ESP_LOGE(TAG, "queue alloc failed");
		abort();                     // 失败即停机，不要带病运行
	}

	// 双核平台明确亲和性；栈深单位仍是 word
	BaseType_t ok = xTaskCreatePinnedToCore(&Network::TaskEntry, "network",
		TASK_STACK_WORDS, this, TASK_PRIORITY, &task_, 0);
	if (ok != pdPASS) {
		ESP_LOGE(TAG, "task alloc failed");
		abort();
	}
}

void Network::TaskLoop() {
	Item item = {};
	for (;;) {
		if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdPASS) {
			continue;
		}
		// 与静态版本完全一致
	}
}
```

要点：

- 用 `xTaskCreatePinnedToCore` 而不是 `xTaskCreate`：双核平台不指定核 = 调度器随机放，通信中断相关任务尤其要钉住；
- 大块缓冲用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`，DMA 缓冲用 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`；
- 任务栈放 PSRAM 需要 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`，且**中断相关任务不要放外部 RAM**；
- 动态创建后仍然遵守「运行期不 delete」：对象生命周期 = 整个固件生命周期。

## 静态创建与内存

以下是**选择静态策略时**的写法：

- 任务：`xTaskCreateStatic` + `StaticTask_t` + `StackType_t[]`；
- 队列：`xQueueCreateStatic` + `StaticQueue_t` + `alignas(8) uint8_t[]`；
- 信号量：`xSemaphoreCreateBinaryStatic` / `xSemaphoreCreateMutexStatic` / `xSemaphoreCreateCountingStatic`；
- 定时器：`xTimerCreateStatic`。

栈深单位是 **word**（Cortex-M 上 4 字节）。队列存储区按 `portBYTE_ALIGNMENT` 对齐，C++ 用 `alignas(8)`，不要裸 `uint8_t[]`。

### 大块数据不要塞进队列

队列是**值拷贝**，与用不用静态分配无关。把几十字节的载荷直接塞进队列元素，会同时吃 RAM 和拷贝时间：

| 数据 | 做法 |
|---|---|
| ≤ 16 字节控制命令 | 直接放队列元素 |
| 几十～几百字节报文 | 预分配 buffer 池 + 队列只传槽位号（`Network` 示例就是这么做的） |
| 连续字节流（串口） | `stream_buffer` / `xStreamBufferCreateStatic` |
| 大图 / 大文件 | 传 Flash 地址或分块 + 起始/长度，不要拷贝 |

## 任务设计

### 一个 APP 类一个任务

- 好：`network`（连接 + 收发）、`display`（绘制）、`storage`（写 Flash）、`sensor`（采集 + 滤波）；
- 差：`uart1_task`、`gpio_task` —— 外设是资源，不是职责；
- 一个 APP 类内部应当是**顺序可读**的阻塞逻辑：`xQueueReceive` → 处理 → 再等。

### 优先级

| 任务 | 优先级 | 说明 |
|---|---|---|
| 硬实时控制 | 5 | 周期 + `vTaskDelayUntil` |
| `main`（MainEventLoop） | 4 | 只派发，必须快速返回 |
| `network` / 协议 | 3 | 阻塞在队列 |
| `storage` / 采集 | 2 | 队列或周期 |
| `display` / LED | 1 | 慢速、可被抢占 |
| Idle | 0 | 系统 |

- 优先级表达**实时性差异**，不是重要性；
- 高优先级任务必须**会阻塞**，否则低优先级任务饿死；
- 软件定时器任务优先级（`configTIMER_TASK_PRIORITY`）必须高于所有使用定时器的任务。

### 周期任务与事件混合

```cpp
void Display::TaskLoop() {
	TickType_t last_wake = xTaskGetTickCount();
	const TickType_t period = pdMS_TO_TICKS(100);

	for (;;) {
		Item item = {};
		// 有事件就处理，没事件就等到下一个周期
		if (xQueueReceive(queue_, &item, period) == pdPASS) {
			Handle(item);
		}
		Refresh();
		vTaskDelayUntil(&last_wake, period);
	}
}
```

- **不要**用 `vTaskDelay` 做周期（处理耗时会累积漂移）；
- **不要** `while (!flag) {}` 忙等；
- 任务里不要出现 `HAL_Delay`，等待一律 `vTaskDelay` / 队列超时。

### 栈大小

- 起点：纯逻辑 256 word，带 `printf`/浮点 512～1024 word；
- 调试期开 `configCHECK_FOR_STACK_OVERFLOW = 2`，跑 10 s 后打印 `uxTaskGetStackHighWaterMark(handle)`，按峰值留 1.5～2 倍余量再定值。

## 同步原语怎么选

| 需求 | 用什么 | 注意 |
|---|---|---|
| APP 之间 / 任务之间传数据 | 队列 | 值拷贝；大块数据传索引 |
| APP 对外暴露能力 | **异步 API + 自己的队列** | 本技能默认做法 |
| 通知"有事件"（无数据） | 二值信号量 / 任务通知 | 任务通知最快，一对一优先 |
| ISR 通知任务 | `xSemaphoreGiveFromISR` / `xTaskNotifyFromISR` | 必须 FromISR 版本 |
| 保护共享外设（I2C/SPI 总线） | 互斥量 `xSemaphoreCreateMutexStatic` | 有优先级继承；ISR 不能用 |
| 计数资源（buffer 池） | 计数信号量 | 初值 = 可用数量 |
| 多条件同时满足 | 事件组 | 位语义 |
| 字节流 | Stream Buffer | 与队列分工 |

### 互斥量的边界

- 只在**任务**里 take，give 必须成对；
- 不要持有互斥量做阻塞操作（等队列、写 Flash、`HAL_Delay`）；
- 嵌套获取用递归互斥量，不要用普通互斥量。

### 临界区

```cpp
taskENTER_CRITICAL();
shared_counter_++;
taskEXIT_CRITICAL();
```

只用于极短、同核、不与 RTOS API 交叉的代码；不要跨函数或阻塞调用持有。

## 软件定时器

- `xTimerCreateStatic` + `xTimerStart`，回调运行在**定时器任务**上下文；
- 回调里不能阻塞，也不能等队列（用 `xQueueSend(..., 0)`）；
- 周期任务能用 `vTaskDelayUntil` 就不用软件定时器。

## FreeRTOSConfig.h 关键项

| 配置 | 建议值 | 原因 |
|---|---|---|
| `configUSE_PREEMPTION` | `1` | 抢占式 |
| `configUSE_TIME_SLICING` | `1` | 同优先级轮转 |
| `configTICK_RATE_HZ` | `1000` | 1 ms 分辨率；需 `configUSE_16_BIT_TICKS = 0` |
| `configMAX_PRIORITIES` | `7` | 够用 |
| `configMINIMAL_STACK_SIZE` | `128` | 空闲任务栈（word） |
| `configSUPPORT_STATIC_ALLOCATION` | 小 RAM：`1`；ESP32：可选 | 是否提供 `x*CreateStatic` |
| `configSUPPORT_DYNAMIC_ALLOCATION` | 小 RAM：`0`；ESP32：`1` | 必须与「分配策略」一致 |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 调试期必开 |
| `configUSE_MALLOC_FAILED_HOOK` | 动态分配时 `1`，静态工程 `0` | 堆失败要能停下来 |
| `configUSE_TICKLESS_IDLE` | `0` | 确认低功耗需求后再开 |
| `INCLUDE_vTaskDelayUntil` | `1` | 周期任务 |
| `INCLUDE_uxTaskGetStackHighWaterMark` | `1` | 定栈大小 |
| `configUSE_NEWLIB_REENTRANT` | `0` | 除非任务里大量用 newlib |

`configASSERT`（调试构建）：

```c
#define configASSERT(x) \
	if ((x) == 0) {     \
		taskDISABLE_INTERRUPTS(); \
		for (;;) {      \
		}               \
	}
```

### 静态分配必须补的回调

`configSUPPORT_DYNAMIC_ALLOCATION = 0`（静态策略）时不需要 `heap_4.c`，但必须提供空闲任务内存（`configUSE_TIMERS = 1` 时还要定时器任务内存）：

```c
// Core/Src/freertos_hooks.c
#include "FreeRTOS.h"
#include "task.h"

static StaticTask_t idle_tcb;
static StackType_t idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *size) {
	*tcb = &idle_tcb;
	*stack = idle_stack;
	*size = configMINIMAL_STACK_SIZE;
}

static StaticTask_t timer_tcb;
static StackType_t timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *size) {
	*tcb = &timer_tcb;
	*stack = timer_stack;
	*size = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
	(void)task;
	(void)name;
	taskDISABLE_INTERRUPTS();
	for (;;) {
	}
}
```

## HAL 与 FreeRTOS 的冲突（必须处理）

这三件事不做，工程能编译但会**死在启动或随机 HardFault**：

1. **SysTick / PendSV / SVC 交给 FreeRTOS**（`FreeRTOSConfig.h`，CubeMX 自动生成）：
   ```c
   #define vPortSVCHandler     SVC_Handler
   #define xPortPendSVHandler  PendSV_Handler
   #define xPortSysTickHandler SysTick_Handler
   ```
   `Core/Src/stm32f4xx_it.c` 里**不能**再定义同名的三个函数。

2. **HAL 时基改由 TIM 提供**：CubeMX 里 `SYS → Timebase Source` 改成 TIM（如 TIM6），否则 `HAL_Delay` / HAL 超时会挂死。

3. **中断优先级与 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 相容**：要通知任务的中断设 5～15。

验证：

```powershell
arm-none-eabi-nm build/<target>/<name>.elf | Select-String "SysTick_Handler|PendSV_Handler|SVC_Handler"
```

每个向量只能有一份定义；`multiple definition` 说明 `*_it.c` 里还留着旧实现。

## 工程配置

### EIDE

FreeRTOS 目录**不要整体丢进 `srcDirs`**：`portable/` 下有十几个移植和 5 个 heap 实现，全部编译会撞符号（`SVC_Handler`、`pvPortMalloc` 重复定义）。用 `virtualFolder` 显式列文件：

```yaml
virtualFolder:
  name: <virtual_root>
  files: []
  folders:
    - name: Middlewares
      files: []
      folders:
        - name: FreeRTOS
          files:
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/tasks.c
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/queue.c
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/list.c
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/timers.c
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
            - path: ../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F/port.c  # AC6
            # 静态分配（configSUPPORT_DYNAMIC_ALLOCATION = 0）时不要加 heap_4.c
          folders: []
targets:
  Debug:
    cppPreprocessAttrs:
      incList:
        # 原有 Core/Drivers/app/bsp/port/devices 路径保持不变，只追加下面几项
        - ../Middlewares/Third_Party/FreeRTOS/Source/include
        - ../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F  # AC6
        - ../Core/Inc                                    # FreeRTOSConfig.h 所在目录
```

- GCC 工程把 `portable/RVDS/ARM_CM4F` 换成 `portable/GCC/ARM_CM4F`；
- 内核源码按 **C** 编译（`language-c: c99`），`app/`、`bsp/` 仍按 C++17 编译。

### Keil MDK（AC6）

- 新建 `Middlewares/FreeRTOS` 组，加入同一份文件清单（`port.c` 选 `RVDS/ARM_CM4F`）；
- `IncludePath` 追加 `../Middlewares/Third_Party/FreeRTOS/Source/include;../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F`；
- 确认 `<Target>` 下有 `<pCCUsed>` + `<uAC6>1</uAC6>`（否则 µVision 用 AC5，C++17 编不过）；
- 内核文件用 `FileType 1`，`app/`、`bsp/` 的 `.cpp` 用 `FileType 8`。

### 改完必须验证

1. 无重复符号（`SVC_Handler` / `PendSV_Handler` / `SysTick_Handler` / `pvPortMalloc`）；
2. 静态分配工程里没有引入 `malloc` / `free`；
3. 运行后 `MainEventLoop` 收到 `START_EVENT`，各 APP 任务按周期/事件跑起来，串口有输出。

## devices / bsp 契约

- `Board` 仍是板级资源与 HAL 回调的唯一入口，单例、私有构造；
- `devices/` 只描述器件行为：不 include 厂商头，也不 include `FreeRTOS.h`；
- **器件接口是同步的**：`module_.Send()`、`panel_.Draw()` 都是阻塞式的一次操作，由调用它的**任务**负责超时与重试；
- APP 是异步的：`network_.Send()` 只是投递，真正调用 `module_.Send()` 的是 `Network::TaskLoop()`；
- 新增板级依赖时，在 `Board` 上加 getter（如 `Get4GModule()`），由 `Application` 注入给需要的 APP 对象。

```cpp
// app/application.cpp —— 依赖注入点
Application::Application()
	: network_(Board::GetInstance().Get4GModule()),
	  display_(Board::GetInstance().GetDisplay()) {}
```

不要在 APP 类内部写 `Board::GetInstance().GetXxx()`：依赖全部从构造函数进，APP 才可替换、可 mock、可单独测试。

## 代码风格

- `app` / `bsp` / `devices` 层：类名 PascalCase（`Application`、`Network`、`Display`、`Led`），方法 PascalCase，成员后置下划线；文件名 snake_case。
- 缩进用 Tab，K&R 花括号，`#pragma once`，注释中文且只写必要的。
- 回调统一无捕获 lambda：`+[](void *ctx) { ... }`；任务入口用静态成员函数。
- RTOS 句柄命名：`xxx_queue_` / `xxx_task_buffer_` / `xxx_task_stack_` / `xxx_sem_`。

## 反模式与正确做法

| 反模式 | 正确做法 |
|---|---|
| APP 公有 API 内部 `xQueueReceive` / 等信号量 / 阻塞发送 | 只投递事件后返回；工作放到自己的任务里 |
| APP 公有 API 用 `xQueueSend(..., portMAX_DELAY)` | 用 `0`，返回"是否入队"，满了计数 |
| 在 `Application` 构造函数里创建任务 | 在 `HandleStart()` 里创建（调度器已启动、单例已构造） |
| APP 对象内部 `Board::GetInstance().GetXxx()` | 构造函数注入，`Application` 负责组装 |
| APP 之间同步互调（`network` 直接调 `display` 的绘制逻辑） | 调对方异步 API，由对方任务执行 |
| 所有事件都塞进 `MainEventLoop` | 模块内部事件走各自队列，主循环只处理中断与跨模块事件 |
| `MainEventLoop` 的 handler 里做重活 | handler 只更新状态 + 投递，重活交给 APP 任务 |
| 队列里塞几十字节以上的载荷 | 传槽位 / 索引 / 地址 / 分块，或用 stream_buffer |
| ISR 里调用 `xQueueSend` / `vTaskDelay` / `printf` | 用 `PostXxxFromISR()`，只投递 |
| 中断优先级 0..4 的 ISR 调用 FromISR API | 会通知任务的中断设为 5～15 |
| `xTaskCreate` / `new` / `std::vector` | 初始化阶段创建一次（小 RAM 用 `xTaskCreateStatic`，大 RAM 可用 `xTaskCreate` 并检查返回值） |
| 任务里 `while (!flag) {}` 忙等 | 阻塞在队列 / 信号量 / 任务通知上 |
| 用 `vTaskDelay` 做周期 | `vTaskDelayUntil` |
| 软件定时器回调里阻塞 | 回调只投递，重活交给任务 |
| 用互斥量保护 ISR 共享数据 | ISR 不能拿互斥量；用队列或短临界区 |
| `devices/` 里调用 `vTaskDelay` 或拿锁 | 阻塞策略留在 APP 任务与 BSP |
| HAL 时基仍是 SysTick | CubeMX 改 Timebase Source 为 TIM |
| 只处理 SysTick，忘了 PendSV / SVC | 三个向量一起交给 FreeRTOS |
| 把整个 `portable/` 加进 `srcDirs` | 显式列 `RVDS|GCC/ARM_CM4F/port.c` |
| 任务栈按字节填 | `xTaskCreateStatic` 的栈深单位是 word |
| 优先级靠猜、从不看水位 | 开溢出检查，用 `uxTaskGetStackHighWaterMark` 定量 |

## 检查清单

- [ ] 中断里只有清标志、搬数据、`...FromISR` 投递，并配了 `portYIELD_FROM_ISR`
- [ ] 所有会通知任务的中断，抢占优先级 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`
- [ ] `Application` 构造函数只建队列/注册回调，任务在 `HandleStart()` 创建
- [ ] `MainEventLoop()` 只派发中断事件与跨模块事件，handler 快速返回
- [ ] 每个 APP 类自己持有队列与任务，公有 API 都是"投递后返回"
- [ ] APP 的依赖全部从构造函数注入，没有内部 `Board::GetInstance()`
- [ ] APP 之间通过异步 API 交互，没有同步互调
- [ ] 任务/队列/信号量/定时器只在初始化阶段创建，运行期不 create/delete
- [ ] 分配策略与平台匹配（小 RAM 静态；ESP32 等可动态），且分配失败可见
- [ ] 没有运行期 `new` / `malloc` / STL 容器反复增删
- [ ] 周期任务用 `vTaskDelayUntil`；没有忙等、没有 `HAL_Delay`
- [ ] 互斥量只在任务里获取，且不跨阻塞调用持有
- [ ] `devices/` 不含 FreeRTOS 与厂商头；器件接口保持同步语义
- [ ] SysTick / PendSV / SVC 只有一个定义，HAL 时基来自 TIM
- [ ] 调试构建打开 `configASSERT` 与 `configCHECK_FOR_STACK_OVERFLOW`
- [ ] 用工程实际工具链完整构建通过（AC6 或 GCC，C++17 + `-fno-exceptions -fno-rtti`）

## CMSIS-RTOS2 对照

用 `cmsis_os2.h` 时概念一一对应，规则不变：

| 本技能（FreeRTOS 原生） | CMSIS-RTOS2 |
|---|---|
| `xTaskCreateStatic` | `osThreadNew` + `osThreadAttr_t`（`.stack_mem` / `.cb_mem`） |
| `xQueueCreateStatic` | `osMessageQueueNew` + `.mq_mem` / `.mq_size` |
| `xQueueSend` / `xQueueSendFromISR` | `osMessageQueuePut(..., timeout)`（ISR 里 timeout 必须 0） |
| `xQueueReceive` | `osMessageQueueGet` |
| `xSemaphoreCreateBinaryStatic` | `osSemaphoreNew(1, 0, &attr)` |
| `xSemaphoreCreateMutexStatic` | `osMutexNew` + `.cb_mem` |
| `xSemaphoreGiveFromISR` | `osSemaphoreRelease`（ISR 安全） |
| `xTaskNotifyFromISR` / `ulTaskNotifyTake` | `osThreadFlagsSet` / `osThreadFlagsWait` |
| `xTimerCreateStatic` | `osTimerNew` + `.cb_mem` |
| `vTaskDelay` / `vTaskDelayUntil` | `osDelay` / `osDelayUntil` |
| `taskENTER_CRITICAL` | 短临界区仍用 `taskENTER_CRITICAL`；长临界区用 `osKernelLock` |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | 同一份 `FreeRTOSConfig.h` 生效 |

CMSIS-RTOS2 只是 API 封装，**中断优先级规则、分配策略、`configASSERT` 全部沿用底层内核约束**。
