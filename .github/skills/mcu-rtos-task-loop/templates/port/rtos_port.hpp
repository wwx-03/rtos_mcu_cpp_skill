#pragma once

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// 统一的 RTOS 薄封装：把会随平台变化的语义显式化。
//
// 约定：
// - 任务栈统一用字节（stack_bytes），平台实现负责换算成 word/byte。
// - StartScheduler() 由平台入口调用一次；ESP-IDF 下为空实现。
// - ISR 版本函数返回 woken，调用方在 ISR 末尾调用 YieldFromIsr(woken)。
// - EnterCritical/ExitCritical 的跨核语义由平台实现决定；双核共享数据请用
//   spinlock/原子操作，不要只依赖 taskENTER_CRITICAL。
// - 动态分配直接用 CreateTask / CreateQueue；静态分配用 StaticTask / StaticQueue。
//   静态存储由对象持有，生命周期必须覆盖任务/队列。
namespace port {

using TaskHandle = void *;
using QueueHandle = void *;
using SemaphoreHandle = void *;
using TimerHandle = void *;
using TaskEntry = void (*)(void *);
using TimerEntry = void (*)(void *);

constexpr uint32_t kNoWait = 0;
// 跨平台“永久等待”值；平台实现负责映射到 portMAX_DELAY。
constexpr uint32_t kWaitForever = 0xFFFFFFFFu;

struct TaskOptions {
	const char *name;
	uint32_t stack_bytes;  // 统一字节，平台实现换算
	void *arg;
	int priority;
};

struct TimerOptions {
	const char *name;
	uint32_t period_ms;
	bool auto_reload;
	void *arg;
};

// 定时器回调上下文；静态/动态 timer 共用。
struct TimerBinding {
	TimerEntry entry = nullptr;
	void *arg = nullptr;
};

// ---- 动态分配（RAM 充足的平台）----

bool CreateTask(TaskEntry entry, const TaskOptions &options, TaskHandle *out);

// 平台入口调用一次。CubeMX：vTaskStartScheduler()；ESP-IDF：空实现。
void StartScheduler();

bool CreateQueue(size_t length, size_t item_size, QueueHandle *out);

// ---- 队列操作 ----

bool QueueSend(QueueHandle queue, const void *item, uint32_t timeout_ms);
bool QueueSendFromIsr(QueueHandle queue, const void *item, bool *woken);
bool QueueReceive(QueueHandle queue, void *item, uint32_t timeout_ms);
uint32_t QueueCount(QueueHandle queue);

// ---- 静态分配（小 RAM 平台）----
// 平台实现用 static_assert 校验 control 大小，不够时用编译选项覆盖这两个宏。
#ifndef PORT_STATIC_TASK_CONTROL_BYTES
#define PORT_STATIC_TASK_CONTROL_BYTES 192
#endif
#ifndef PORT_STATIC_QUEUE_CONTROL_BYTES
#define PORT_STATIC_QUEUE_CONTROL_BYTES 128
#endif
#ifndef PORT_STATIC_TIMER_CONTROL_BYTES
#define PORT_STATIC_TIMER_CONTROL_BYTES 96
#endif

// 平台实现：由 rtos_port.cpp 提供；不支持静态分配时返回 false。
bool CreateTaskStaticRaw(TaskEntry entry, const TaskOptions &options,
	void *control, void *stack, size_t stack_bytes, TaskHandle *out);
bool CreateQueueStaticRaw(size_t length, size_t item_size,
	void *control, void *buffer, QueueHandle *out);

// ---- 信号量 ----

bool CreateBinarySemaphore(SemaphoreHandle *out);
bool CreateMutex(SemaphoreHandle *out);
bool SemaphoreTake(SemaphoreHandle semaphore, uint32_t timeout_ms);
bool SemaphoreGive(SemaphoreHandle semaphore);
bool SemaphoreGiveFromIsr(SemaphoreHandle semaphore, bool *woken);

// 任务通知：一对一事件通知，优先于二值信号量。
bool TaskNotifyGive(TaskHandle task);
bool TaskNotifyGiveFromIsr(TaskHandle task, bool *woken);
// 等待一次通知；返回 true 表示收到并清除计数。
bool TaskNotifyTake(uint32_t timeout_ms);

// ---- 定时器 ----

// 定时器回调运行在 timer service 任务：只投递，不阻塞、不 stop/free 其他资源。
// 实现可能在 Init() 阶段为回调上下文分配内存；禁止 heap 的平台改用固定静态池。
bool CreateTimer(TimerEntry entry, const TimerOptions &options, TimerHandle *out);
// delay_ms 是启动/重置前的阻塞等待时间，不是新的周期。
bool StartTimer(TimerHandle timer, uint32_t delay_ms);
bool StopTimer(TimerHandle timer, uint32_t wait_ms);
bool ResetTimer(TimerHandle timer, uint32_t delay_ms);
// wait_ms 必须 > 0：阻塞等待删除完成后再释放回调上下文；
// 禁止在 timer 回调或 ISR 里调用，否则会死锁或 use-after-free。
bool DeleteTimer(TimerHandle timer, uint32_t wait_ms);

// 静态 timer：control 与回调上下文由对象持有，不依赖 heap。
// Delete() 的 wait_ms 必须 > 0；禁止在回调里调用。
bool CreateTimerStaticRaw(TimerEntry entry, void *arg, const char *name,
	uint32_t period_ms, bool auto_reload, TimerBinding *binding,
	void *control, TimerHandle *out);
// 静态 timer 删除：只删 timer，不释放调用方持有的 binding。
bool DeleteTimerStaticRaw(TimerHandle timer, uint32_t wait_ms);

class StaticTimer {
public:
	bool Create(TimerEntry entry, void *arg, const char *name,
		uint32_t period_ms, bool auto_reload) {
		if (handle_ != nullptr) {
			return false;
		}
		return CreateTimerStaticRaw(entry, arg, name, period_ms, auto_reload,
			&binding_, control_, &handle_);
	}

	bool Start(uint32_t delay_ms = kNoWait) { return StartTimer(handle_, delay_ms); }
	bool Stop(uint32_t wait_ms) { return StopTimer(handle_, wait_ms); }
	bool Reset(uint32_t delay_ms) { return ResetTimer(handle_, delay_ms); }
	bool Delete(uint32_t wait_ms) { return DeleteTimerStaticRaw(handle_, wait_ms); }

	[[nodiscard]] TimerHandle handle() const { return handle_; }

private:
	TimerBinding binding_{};
	alignas(16) uint8_t control_[PORT_STATIC_TIMER_CONTROL_BYTES] = {};
	TimerHandle handle_ = nullptr;
};

// ---- 临界区与 ISR ----

void EnterCritical();
void ExitCritical();
void YieldFromIsr(bool woken);

struct IsrWoken {
	bool value = false;
};

inline void YieldFromIsr(IsrWoken &woken) {
	YieldFromIsr(woken.value);
}

// RAII 临界区：只在极短、同核、不调用 RTOS API 的代码块里使用。
class CriticalSection {
public:
	CriticalSection() { EnterCritical(); }
	~CriticalSection() { ExitCritical(); }
	CriticalSection(const CriticalSection &) = delete;
	CriticalSection &operator=(const CriticalSection &) = delete;
};

// ---- 时间 ----

// 单调时间，用于诊断/日志；不要用它做墙钟。
uint64_t NowMs();

// 64 位单调时间（毫秒），用于长期运行诊断；仅任务上下文调用。
// 32 位 tick 平台内部处理回绕，约 49 天不会倒退。
uint64_t UptimeMs();
void DelayMs(uint32_t ms);

// 周期延时，避免 DelayMs 的累积漂移。
// last_wake_ms 的初值写 0，首次调用会以当前时间初始化。
// 该参数是 32 位毫秒，约 49 天回绕；长期运行诊断请用 NowMs()。
// 周期小于 1 个 tick 时精度受 tick 限制；超期后按当前时间重新对齐，不补跑。
void DelayUntil(uint32_t *last_wake_ms, uint32_t period_ms);

// ---- 诊断 ----

// 返回字节；原生 FreeRTOS 由 port 换算。
uint32_t GetStackHighWaterMark(TaskHandle task);
uint32_t GetFreeHeap();
uint32_t GetMinimumEverFreeHeap();

// ---- 对象封装 ----
// 存储由对象持有：模块对象必须是全局/静态或由 Board 持有。
// 含 StaticTask 的对象带完整栈，**不能放在任务栈上**。

template <uint32_t StackBytes>
class StaticTask {
	static_assert(StackBytes % 8 == 0, "stack size must be a multiple of 8");

public:
	StaticTask() = default;
	StaticTask(const StaticTask &) = delete;
	StaticTask &operator=(const StaticTask &) = delete;

	bool Start(TaskEntry entry, void *ctx, const char *name, uint8_t priority) {
		if (handle_ != nullptr) {
			return false;
		}
		TaskOptions options{name, StackBytes, ctx, static_cast<int>(priority)};
		return CreateTaskStaticRaw(entry, options, control_, stack_, StackBytes, &handle_);
	}

	[[nodiscard]] TaskHandle handle() const { return handle_; }

	[[nodiscard]] uint32_t StackHeadroomBytes() const {
		return handle_ == nullptr ? 0 : GetStackHighWaterMark(handle_);
	}

private:
	alignas(16) uint8_t control_[PORT_STATIC_TASK_CONTROL_BYTES] = {};
	alignas(16) uint8_t stack_[StackBytes] = {};
	TaskHandle handle_ = nullptr;
};

template <typename T, uint32_t Length>
class StaticQueue {
	static_assert(Length > 0, "queue length must be > 0");
	static_assert(std::is_trivially_copyable<T>::value, "queue item must be trivially copyable");

public:
	StaticQueue() = default;
	StaticQueue(const StaticQueue &) = delete;
	StaticQueue &operator=(const StaticQueue &) = delete;

	bool Create() {
		if (handle_ != nullptr) {
			return false;
		}
		return CreateQueueStaticRaw(Length, sizeof(T), control_, storage_, &handle_);
	}

	[[nodiscard]] bool valid() const { return handle_ != nullptr; }

	bool Send(const T &item, uint32_t wait_ms = kNoWait) {
		return QueueSend(handle_, &item, wait_ms);
	}

	bool Receive(T &item, uint32_t wait_ms = kWaitForever) {
		return QueueReceive(handle_, &item, wait_ms);
	}

	bool SendFromIsr(const T &item, IsrWoken &woken) {
		return QueueSendFromIsr(handle_, &item, &woken.value);
	}

	[[nodiscard]] uint32_t Count() const { return QueueCount(handle_); }
	[[nodiscard]] QueueHandle handle() const { return handle_; }

private:
	alignas(16) uint8_t control_[PORT_STATIC_QUEUE_CONTROL_BYTES] = {};
	alignas(16) uint8_t storage_[Length * sizeof(T)] = {};
	QueueHandle handle_ = nullptr;
};

class PeriodicTimer {
public:
	explicit PeriodicTimer(uint32_t period_ms) : period_ms_(period_ms), last_wake_ms_(0) {}

	void WaitNext() { DelayUntil(&last_wake_ms_, period_ms_); }

private:
	uint32_t period_ms_;
	uint32_t last_wake_ms_;
};

}  // namespace port