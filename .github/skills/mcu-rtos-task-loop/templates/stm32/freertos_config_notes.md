# FreeRTOSConfig.h 关键项（STM32）

| 配置 | 建议值 | 原因 |
| --- | --- | --- |
| `configUSE_PREEMPTION` | `1` | 抢占式 |
| `configUSE_TIME_SLICING` | `1` | 同优先级轮转 |
| `configTICK_RATE_HZ` | `1000` | 1 ms 分辨率；需 `configUSE_16_BIT_TICKS = 0` |
| `configMAX_PRIORITIES` | `7` | 够用 |
| `configMINIMAL_STACK_SIZE` | `128` | 空闲任务栈（word） |
| `configSUPPORT_STATIC_ALLOCATION` | 小 RAM：`1` | 静态创建 |
| `configSUPPORT_DYNAMIC_ALLOCATION` | 小 RAM：`0` | 与策略一致 |
| `configUSE_TASK_NOTIFICATIONS` | `1` | `port::TaskNotify*` 与一对一事件通知 |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 调试期必开 |
| `configUSE_MALLOC_FAILED_HOOK` | 动态分配时 `1` | 堆失败要能停下来 |
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

## 静态分配必须补的回调

`configSUPPORT_DYNAMIC_ALLOCATION = 0` 时不需要 `heap_4.c`，但必须提供空闲任务内存
（`configUSE_TIMERS = 1` 时还要定时器任务内存）：

```c
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
