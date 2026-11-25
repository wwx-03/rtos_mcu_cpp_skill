# FreeRTOSConfig.h 关键项（STM32）

| 配置 | 建议值 | 原因 |
| --- | --- | --- |
| `configUSE_PREEMPTION` | `1` | 抢占式 |
| `configUSE_TIME_SLICING` | `1` | 同优先级轮转 |
| `configTICK_RATE_HZ` | `1000` | 1 ms 分辨率；需 `configUSE_16_BIT_TICKS = 0` |
| `configMAX_PRIORITIES` | `7` | 够用 |
| `configMINIMAL_STACK_SIZE` | `128` | 空闲任务栈（word） |
| `configSUPPORT_DYNAMIC_ALLOCATION` | `1` | 启动一次性从 RTOS 堆分配 |
| `configSUPPORT_STATIC_ALLOCATION` | `0` | 不做逐对象静态创建 |
| `configTOTAL_HEAP_SIZE` | 剩余 RAM - 保留 | 见下一节 |
| `configUSE_TASK_NOTIFICATIONS` | `1` | `xTaskNotifyGive` / `ulTaskNotifyTake` |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 调试期必开 |
| `configUSE_MALLOC_FAILED_HOOK` | `1` | 堆失败要能停下来 |
| `configUSE_TICKLESS_IDLE` | `0` | 确认低功耗需求后再开 |
| `INCLUDE_vTaskDelayUntil` | `1` | 周期任务 |
| `INCLUDE_uxTaskGetStackHighWaterMark` | `1` | 定栈大小 |
| `configUSE_NEWLIB_REENTRANT` | `0` | 除非任务里大量用 newlib |

## 堆大小怎么定

**先把不用 RTOS 堆的 RAM 布置完，再把剩余 RAM 给 `configTOTAL_HEAP_SIZE`。**

1. 从 `size` / map 文件读出 `.data`、`.bss`（含全局对象、显示 framebuffer、DMA 缓冲）；
2. 加上主栈（MSP）与启动栈；
3. 加上保留（10% 或 1~2 KB，取大者）；
4. 剩余 RAM 全部给 `configTOTAL_HEAP_SIZE`，并计入 idle 任务与 timer 任务的栈
   （`configMINIMAL_STACK_SIZE`、`configTIMER_TASK_STACK_DEPTH`）。

```c
#define configSUPPORT_DYNAMIC_ALLOCATION  1
#define configSUPPORT_STATIC_ALLOCATION   0
#define configTOTAL_HEAP_SIZE   ((size_t)(100 * 1024))
#define configUSE_MALLOC_FAILED_HOOK       1
```

- `ucHeap` 落在 `.bss`：填太大直接链接失败，比运行期才发现更容易定位；
- 运行期不再 create/delete/free，堆水位在启动后应保持稳定；
- RAM 不连续（多块 SRAM / CCM / DTCM）时用 `heap_5.c`，启动最早期调用
  `vPortDefineHeapRegions()`，**之后**再创建任何 RTOS 对象；
- CCM/DTCM 不能给 DMA，划分区域前先查芯片手册。

## 钩子函数

动态分配时**不需要** `vApplicationGetIdleTaskMemory` / `vApplicationGetTimerTaskMemory`；
idle 与 timer 任务的内存同样来自 RTOS 堆，要计入预算。

```c
void vApplicationMallocFailedHook(void) {
	taskDISABLE_INTERRUPTS();
	for (;;) {
	}
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
	(void)task;
	(void)name;
	taskDISABLE_INTERRUPTS();
	for (;;) {
	}
}

#define configASSERT(x) \
	if ((x) == 0) {     \
		taskDISABLE_INTERRUPTS(); \
		for (;;) {      \
		}               \
	}
```

调试构建里 `configASSERT` 必须打开；量产可以把钩子换成记录 + 复位。