#include "port/rtos_port.hpp"

#include <new>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

namespace port {

namespace {

struct TimerBinding {
	TimerEntry entry;
	void *arg;
};

void TimerTrampoline(TimerHandle_t timer) {
	auto *binding = static_cast<TimerBinding *>(pvTimerGetTimerID(timer));
	if (binding != nullptr && binding->entry != nullptr) {
		binding->entry(binding->arg);
	}
}

TickType_t ToTicks(uint32_t timeout_ms) {
	return timeout_ms == kWaitForever ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

}  // namespace

bool CreateTask(TaskEntry entry, const TaskOptions &options, TaskHandle *out) {
	if (out == nullptr || options.name == nullptr || options.stack_bytes == 0) {
		return false;
	}

	TaskHandle_t freertos_handle = nullptr;
	// 原生 FreeRTOS 的栈深单位是 word，这里统一按字节换算。
	const uint32_t stack_words = options.stack_bytes / sizeof(StackType_t);
	const BaseType_t ok = xTaskCreate(entry, options.name, stack_words,
		options.arg, static_cast<UBaseType_t>(options.priority), &freertos_handle);
	*out = static_cast<TaskHandle>(freertos_handle);
	return ok == pdPASS;
}

void StartScheduler() {
	vTaskStartScheduler();
	for (;;) {
		// 返回说明任务创建失败/内存不足。
	}
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

bool CreateTimer(TimerEntry entry, const TimerOptions &options, TimerHandle *out) {
	if (out == nullptr || entry == nullptr || options.name == nullptr || options.period_ms == 0) {
		return false;
	}

	// binding 在 init 阶段分配；若平台禁止 heap，改用固定大小静态池。
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
	if (timer == nullptr) {
		return false;
	}
	auto *handle = static_cast<TimerHandle_t>(timer);
	auto *binding = static_cast<TimerBinding *>(pvTimerGetTimerID(handle));
	const BaseType_t ok = xTimerDelete(handle, ToTicks(wait_ms));
	if (ok == pdPASS) {
		delete binding;
	}
	return ok == pdPASS;
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

uint64_t NowMs() {
	return static_cast<uint64_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
}

void DelayMs(uint32_t ms) {
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void DelayUntil(uint32_t *last_wake_ms, uint32_t period_ms) {
	if (last_wake_ms == nullptr) {
		DelayMs(period_ms);
		return;
	}
	const TickType_t period_ticks = pdMS_TO_TICKS(period_ms);
	if (period_ticks == 0) {
		return;
	}
	if (*last_wake_ms == 0) {
		*last_wake_ms = static_cast<uint32_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
	}
	TickType_t last_wake_tick = pdMS_TO_TICKS(*last_wake_ms);
	xTaskDelayUntil(&last_wake_tick, period_ticks);
	*last_wake_ms = static_cast<uint32_t>(last_wake_tick) * portTICK_PERIOD_MS;
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
