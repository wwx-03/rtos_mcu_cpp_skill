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
using TaskEntry = void (*)(void *);

struct TaskOptions {
	const char *name;
	uint32_t stack_bytes;  // 统一字节，平台实现换算
	void *arg;
	int priority;
};

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

void EnterCritical();
void ExitCritical();
void YieldFromIsr(bool woken);

// 单调时间，用于诊断/日志；不要用它做墙钟。
uint64_t NowMs();
void DelayMs(uint32_t ms);

// 周期延时，避免 DelayMs 的累积漂移。
// last_wake_ms 的初值写 0，首次调用会以当前时间初始化。
void DelayUntil(uint32_t *last_wake_ms, uint32_t period_ms);

}  // namespace port
