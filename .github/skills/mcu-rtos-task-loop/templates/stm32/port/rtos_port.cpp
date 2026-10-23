#include "port/rtos_port.hpp"

#include <new>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

namespace port {

namespace {

void TimerTrampoline(TimerHandle_t timer) {
	auto *binding = static_cast<TimerBinding *>(pvTimerGetTimerID(timer));
	if (binding != nullptr && binding->entry != nullptr) {
		binding->entry(binding->arg);
	}
}

TickType_t ToTicks(uint32_t timeout_ms) {
	return timeout_ms == kWaitForever ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

#if (configSUPPORT_STATIC_ALLOCATION == 1)
static_assert(sizeof(StaticTask_t) <= PORT_STATIC_TASK_CONTROL_BYTES,
	"PORT_STATIC_TASK_CONTROL_BYTES is too small for StaticTask_t");
static_assert(sizeof(StaticQueue_t) <= PORT_STATIC_QUEUE_CONTROL_BYTES,
	"PORT_STATIC_QUEUE_CONTROL_BYTES is too small for StaticQueue_t");
static_assert(sizeof(StaticTimer_t) <= PORT_STATIC_TIMER_CONTROL_BYTES,
	"PORT_STATIC_TIMER_CONTROL_BYTES is too small for StaticTimer_t");
#endif

}  // namespace

bool CreateTask(TaskEntry entry, const TaskOptions &options, TaskHandle *out) {
	if (out == nullptr || options.name == nullptr || options.stack_bytes == 0) {
		return false;
	}

	TaskHandle_t freertos_handle = nullptr;
	// 原生 FreeRTOS 的栈深单位是 word，这里统一按字节换算。
	const uint32_t stack_words =
		(options.stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);
	const BaseType_t ok = xTaskCreate(entry, options.name, stack_words,
		options.arg, static_cast<UBaseType_t>(options.priority), &freertos_handle);
	*out = static_cast<TaskHandle>(freertos_handle);
	return ok == pdPASS;
}

void StartScheduler() {
	vTaskStartScheduler();
	// 返回说明堆/任务创建失败。关中断后停住，避免空循环被优化掉后继续执行。
	portDISABLE_INTERRUPTS();
	for (;;) {
	}
}

bool CreateTaskStaticRaw(TaskEntry entry, const TaskOptions &options,
	void *control, void *stack, size_t stack_bytes, TaskHandle *out) {
	if (out == nullptr) {
		return false;
	}
	*out = nullptr;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
	if (entry == nullptr || control == nullptr || stack == nullptr ||
		options.name == nullptr || stack_bytes == 0 ||
		(stack_bytes % sizeof(StackType_t)) != 0) {
		return false;
	}
	// 原生 FreeRTOS 的 xTaskCreateStatic 栈深单位是 word。
	const uint32_t stack_words =
		static_cast<uint32_t>(stack_bytes / sizeof(StackType_t));
	TaskHandle_t handle = xTaskCreateStatic(entry, options.name, stack_words,
		options.arg, static_cast<UBaseType_t>(options.priority),
		static_cast<StackType_t *>(stack),
		static_cast<StaticTask_t *>(control));
	*out = static_cast<TaskHandle>(handle);
	return handle != nullptr;
#else
	(void)entry;
	(void)options;
	(void)control;
	(void)stack;
	(void)stack_bytes;
	return false;
#endif
}

bool CreateQueue(size_t length, size_t item_size, QueueHandle *out) {
	if (out == nullptr || length == 0 || item_size == 0) {
		return false;
	}
	const QueueHandle_t handle = xQueueCreate(static_cast<UBaseType_t>(length),
		static_cast<UBaseType_t>(item_size));
	*out = static_cast<QueueHandle>(handle);
	return handle != nullptr;
}

bool CreateQueueStaticRaw(size_t length, size_t item_size,
	void *control, void *buffer, QueueHandle *out) {
	if (out == nullptr) {
		return false;
	}
	*out = nullptr;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
	if (control == nullptr || buffer == nullptr || length == 0 || item_size == 0) {
		return false;
	}
	QueueHandle_t handle = xQueueCreateStatic(static_cast<UBaseType_t>(length),
		static_cast<UBaseType_t>(item_size), static_cast<uint8_t *>(buffer),
		static_cast<StaticQueue_t *>(control));
	*out = static_cast<QueueHandle>(handle);
	return handle != nullptr;
#else
	(void)length;
	(void)item_size;
	(void)control;
	(void)buffer;
	return false;
#endif
}

uint32_t QueueCount(QueueHandle queue) {
	if (queue == nullptr) {
		return 0;
	}
	return static_cast<uint32_t>(uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue)));
}

bool QueueSend(QueueHandle queue, const void *item, uint32_t timeout_ms) {
	if (queue == nullptr || item == nullptr) {
		return false;
	}
	return xQueueSend(static_cast<QueueHandle_t>(queue), item, ToTicks(timeout_ms)) == pdPASS;
}

bool QueueSendFromIsr(QueueHandle queue, const void *item, bool *woken) {
	if (queue == nullptr || item == nullptr || woken == nullptr) {
		return false;
	}
	BaseType_t woken_flag = pdFALSE;
	const BaseType_t ok = xQueueSendFromISR(static_cast<QueueHandle_t>(queue), item, &woken_flag);
	*woken = (woken_flag == pdTRUE);
	return ok == pdPASS;
}

bool QueueReceive(QueueHandle queue, void *item, uint32_t timeout_ms) {
	if (queue == nullptr || item == nullptr) {
		return false;
	}
	return xQueueReceive(static_cast<QueueHandle_t>(queue), item, ToTicks(timeout_ms)) == pdPASS;
}

bool CreateBinarySemaphore(SemaphoreHandle *out) {
	if (out == nullptr) {
		return false;
	}
	const SemaphoreHandle_t handle = xSemaphoreCreateBinary();
	*out = static_cast<SemaphoreHandle>(handle);
	return handle != nullptr;
}

bool CreateMutex(SemaphoreHandle *out) {
	if (out == nullptr) {
		return false;
	}
	const SemaphoreHandle_t handle = xSemaphoreCreateMutex();
	*out = static_cast<SemaphoreHandle>(handle);
	return handle != nullptr;
}

bool SemaphoreTake(SemaphoreHandle semaphore, uint32_t timeout_ms) {
	if (semaphore == nullptr) {
		return false;
	}
	return xSemaphoreTake(static_cast<SemaphoreHandle_t>(semaphore), ToTicks(timeout_ms)) == pdTRUE;
}

bool SemaphoreGive(SemaphoreHandle semaphore) {
	if (semaphore == nullptr) {
		return false;
	}
	return xSemaphoreGive(static_cast<SemaphoreHandle_t>(semaphore)) == pdTRUE;
}

bool SemaphoreGiveFromIsr(SemaphoreHandle semaphore, bool *woken) {
	if (semaphore == nullptr || woken == nullptr) {
		return false;
	}
	BaseType_t woken_flag = pdFALSE;
	const BaseType_t ok = xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(semaphore), &woken_flag);
	*woken = (woken_flag == pdTRUE);
	return ok == pdTRUE;
}

bool TaskNotifyGive(TaskHandle task) {
	if (task == nullptr) {
		return false;
	}
	return xTaskNotifyGive(static_cast<TaskHandle_t>(task)) == pdPASS;
}

bool TaskNotifyGiveFromIsr(TaskHandle task, bool *woken) {
	if (task == nullptr || woken == nullptr) {
		return false;
	}
	BaseType_t woken_flag = pdFALSE;
	vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(task), &woken_flag);
	*woken = (woken_flag == pdTRUE);
	return true;
}

bool TaskNotifyTake(uint32_t timeout_ms) {
	return ulTaskNotifyTake(pdTRUE, ToTicks(timeout_ms)) > 0;
}

bool CreateTimerStaticRaw(TimerEntry entry, void *arg, const char *name,
	uint32_t period_ms, bool auto_reload, TimerBinding *binding,
	void *control, TimerHandle *out) {
	if (out == nullptr) {
		return false;
	}
	*out = nullptr;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
	if (entry == nullptr || name == nullptr || period_ms == 0 || binding == nullptr ||
		control == nullptr) {
		return false;
	}
	binding->entry = entry;
	binding->arg = arg;
	TimerHandle_t handle = xTimerCreateStatic(name, pdMS_TO_TICKS(period_ms),
		auto_reload ? pdTRUE : pdFALSE, binding, TimerTrampoline,
		static_cast<StaticTimer_t *>(control));
	*out = static_cast<TimerHandle>(handle);
	return handle != nullptr;
#else
	(void)entry;
	(void)arg;
	(void)name;
	(void)period_ms;
	(void)auto_reload;
	(void)binding;
	(void)control;
	return false;
#endif
}

bool CreateTimer(TimerEntry entry, const TimerOptions &options, TimerHandle *out) {
	if (out == nullptr || entry == nullptr || options.name == nullptr || options.period_ms == 0) {
		return false;
	}
	*out = nullptr;

	// binding 在 Init() 阶段分配；若平台禁止 heap，改用固定大小静态池。
	auto *binding = new (std::nothrow) TimerBinding{entry, options.arg};
	if (binding == nullptr) {
		return false;
	}

	const TimerHandle_t handle = xTimerCreate(options.name, pdMS_TO_TICKS(options.period_ms),
		options.auto_reload ? pdTRUE : pdFALSE, binding, TimerTrampoline);
	if (handle == nullptr) {
		delete binding;
		*out = nullptr;
		return false;
	}

	*out = static_cast<TimerHandle>(handle);
	return true;
}

bool StartTimer(TimerHandle timer, uint32_t delay_ms) {
	if (timer == nullptr) {
		return false;
	}
	return xTimerStart(static_cast<TimerHandle_t>(timer), ToTicks(delay_ms)) == pdPASS;
}

bool StopTimer(TimerHandle timer, uint32_t wait_ms) {
	if (timer == nullptr) {
		return false;
	}
	return xTimerStop(static_cast<TimerHandle_t>(timer), ToTicks(wait_ms)) == pdPASS;
}

bool ResetTimer(TimerHandle timer, uint32_t delay_ms) {
	if (timer == nullptr) {
		return false;
	}
	return xTimerReset(static_cast<TimerHandle_t>(timer), ToTicks(delay_ms)) == pdPASS;
}

bool DeleteTimer(TimerHandle timer, uint32_t wait_ms) {
	if (timer == nullptr || wait_ms == 0) {
		return false;
	}
	// xTimerDelete 只是把命令投递到 timer 任务；wait_ms == 0 时无法保证回调已停止，
	// 立刻释放 binding 会 use-after-free。必须阻塞等待删除完成；禁止在 timer 回调里调用。
	auto *handle = static_cast<TimerHandle_t>(timer);
	auto *binding = static_cast<TimerBinding *>(pvTimerGetTimerID(handle));
	const BaseType_t ok = xTimerDelete(handle, ToTicks(wait_ms));
	if (ok != pdPASS) {
		return false;
	}
	delete binding;
	return true;
}

bool DeleteTimerStaticRaw(TimerHandle timer, uint32_t wait_ms) {
	if (timer == nullptr || wait_ms == 0) {
		return false;
	}
	return xTimerDelete(static_cast<TimerHandle_t>(timer), ToTicks(wait_ms)) == pdPASS;
}

void EnterCritical() {
	taskENTER_CRITICAL();
}

void ExitCritical() {
	taskEXIT_CRITICAL();
}

void YieldFromIsr(bool woken) {
	if (woken) {
		portYIELD_FROM_ISR(pdTRUE);
	}
}

uint64_t UptimeMs() {
	// 32 位 tick 回绕：高字累加。只在任务上下文调用，临界区避免多任务竞争。
	static uint32_t last_tick = 0;
	static uint64_t high = 0;

	EnterCritical();
	const uint32_t tick = static_cast<uint32_t>(xTaskGetTickCount());
	if (tick < last_tick) {
		high += (1ULL << 32);
	}
	last_tick = tick;
	ExitCritical();

	return high + static_cast<uint64_t>(tick) * portTICK_PERIOD_MS;
}

uint64_t NowMs() {
	return UptimeMs();
}

void DelayMs(uint32_t ms) {
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void DelayUntil(uint32_t *last_wake_ms, uint32_t period_ms) {
	if (last_wake_ms == nullptr || period_ms == 0) {
		return;
	}
	const uint32_t now = static_cast<uint32_t>(NowMs());
	if (*last_wake_ms == 0) {
		*last_wake_ms = now;
	}
	const uint32_t next = *last_wake_ms + period_ms;
	const int32_t wait_ms = static_cast<int32_t>(next - now);
	if (wait_ms > 0) {
		DelayMs(static_cast<uint32_t>(wait_ms));
		*last_wake_ms = next;
	} else {
		// 已经超期：不补跑，按当前时间重新对齐，避免突发。
		*last_wake_ms = static_cast<uint32_t>(NowMs());
	}
}

uint32_t GetStackHighWaterMark(TaskHandle task) {
	// 原生 FreeRTOS 返回 word，这里统一换算为字节。
	return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(static_cast<TaskHandle_t>(task)))
		* sizeof(StackType_t);
}

// heap_1/heap_2 等实现可能不提供 free/minimum-ever 统计；
// 按实际使用的 heap_*.c 裁剪，或返回 0。
uint32_t GetFreeHeap() {
#if (configSUPPORT_DYNAMIC_ALLOCATION == 1)
	return static_cast<uint32_t>(xPortGetFreeHeapSize());
#else
	return 0;
#endif
}

uint32_t GetMinimumEverFreeHeap() {
#if (configSUPPORT_DYNAMIC_ALLOCATION == 1)
	return static_cast<uint32_t>(xPortGetMinimumEverFreeHeapSize());
#else
	return 0;
#endif
}

}  // namespace port
