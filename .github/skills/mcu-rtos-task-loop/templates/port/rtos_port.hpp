#pragma once

#include <stddef.h>
#include <stdint.h>

// 统一的 RTOS 薄封装：把会随平台变化的语义显式化。
//
// 约定：
// - 任务栈统一用字节（stack_bytes），平台实现负责换算成 word/byte。
// - StartScheduler() 由平台入口调用一次；ESP-IDF 下为空实现。
// - ISR 版本函数返回 woken，调用方在 ISR 末尾调用 YieldFromIsr(woken)。
// - EnterCritical/ExitCritical 的跨核语义由平台实现决定；双核共享数据请用
//   spinlock/原子操作，不要只依赖 taskENTER_CRITICAL。
namespace port {

using TaskHandle = void *;
using QueueHandle = void *;
using SemaphoreHandle = void *;
using TimerHandle = void *;
using TaskEntry = void (*)(void *);
using TimerEntry = void (*)(void *);

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

// 静态分配：不同平台差异较大，模板不提供统一 API。
// 需要静态创建时，在平台 port 中按 SDK 语义包装 xTaskCreateStatic /
// xQueueCreateStatic / xTimerCreateStatic，并在平台契约中记录缓冲区类型、
// 对齐要求和栈单位。不要把 xTaskCreateStatic 的 word/byte 语义直接套到其他平台。

bool CreateTask(TaskEntry entry, const TaskOptions &options, TaskHandle *out);

// 平台入口调用一次。CubeMX：vTaskStartScheduler()；ESP-IDF：空实现。
void StartScheduler();

bool CreateQueue(size_t length, size_t item_size, QueueHandle *out);
bool QueueSend(QueueHandle queue, const void *item, uint32_t timeout_ms);
bool QueueSendFromIsr(QueueHandle queue, const void *item, bool *woken);
bool QueueReceive(QueueHandle queue, void *item, uint32_t timeout_ms);

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

// 定时器回调运行在 timer service 任务：只投递，不阻塞、不 stop/free 其他资源。
bool CreateTimer(TimerEntry entry, const TimerOptions &options, TimerHandle *out);
// delay_ms 是启动/重置前的阻塞等待时间，不是新的周期。
bool StartTimer(TimerHandle timer, uint32_t delay_ms);
bool StopTimer(TimerHandle timer, uint32_t wait_ms);
bool ResetTimer(TimerHandle timer, uint32_t delay_ms);
bool DeleteTimer(TimerHandle timer, uint32_t wait_ms);

void EnterCritical();
void ExitCritical();
void YieldFromIsr(bool woken);

// 单调时间，用于诊断/日志；不要用它做墙钟。
uint64_t NowMs();
void DelayMs(uint32_t ms);

// 周期延时，避免 DelayMs 的累积漂移。
// last_wake_ms 的初值写 0，首次调用会以当前时间初始化。
// 该参数是 32 位毫秒，约 49 天回绕；长期运行诊断请用 NowMs()。
void DelayUntil(uint32_t *last_wake_ms, uint32_t period_ms);

// 诊断：ESP-IDF 下栈高水位返回字节；原生 FreeRTOS 由 port 换算为字节。
uint32_t GetStackHighWaterMark(TaskHandle task);
uint32_t GetFreeHeap();
uint32_t GetMinimumEverFreeHeap();

}  // namespace port
