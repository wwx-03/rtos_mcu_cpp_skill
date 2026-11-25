# STM32 / CubeMX 适配参考

适用于 STM32 + CubeMX + FreeRTOS（AC6 / GCC），也适用于大多数 Cortex-M 工程。

## 1. 启动模型

```cpp
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();
    // ... 外设初始化 ...

    if (!Board::GetInstance().Init()) {
        Error_Handler();
    }
    Application &app = Application::GetInstance();
    if (!app.Init()) {          // 建队列/信号量/定时器
        Error_Handler();
    }
    if (!app.Start()) {         // 创建任务
        Error_Handler();
    }
    vTaskStartScheduler();      // 只调用一次
    for (;;) {
    }
}
```

- `Application::Start()` 可以创建任务，但**调度器只由平台入口启动一次**；
- `vTaskStartScheduler()` 返回说明内存不足/任务创建失败，要有兜底；
- 单例首次构造放在 `vTaskStartScheduler()` 之前。

## 2. HAL 与 FreeRTOS 的冲突

这三件事不做，工程能编译但会死在启动或随机 HardFault：

1. **SysTick / PendSV / SVC 交给 FreeRTOS**
   ```c
   #define vPortSVCHandler     SVC_Handler
   #define xPortPendSVHandler  PendSV_Handler
   #define xPortSysTickHandler SysTick_Handler
   ```
   `stm32xxxx_it.c` 里不能再定义同名函数；否则 `multiple definition`。

2. **HAL 时基改由 TIM 提供**  
   CubeMX：`SYS → Timebase Source` 改成 TIM（如 TIM6）。
   否则 `HAL_Delay` / HAL 超时会与 FreeRTOS 的 SysTick 冲突或挂死。

3. **中断优先级与 ISR API 相容**  
   调用 `...FromISR` 的中断，其 NVIC 抢占优先级数值必须
   ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`（Cortex-M 上数值越大优先级越低）。

   ```c
   HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);   // 5..15 才能调用 FromISR API
   HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
   ```

   - 优先级 0..4 的中断不可以调用任何 RTOS API；
   - NVIC 优先级分组必须是全抢占位（`NVIC_PRIORITYGROUP_4`）；
   - 调试期开 `configASSERT`，`vPortValidateInterruptPriority()` 会抓违规。

## 3. 栈单位

原生 FreeRTOS 的 `xTaskCreate` / `xTaskCreateStatic` 栈深单位是 **word**：

```c
xTaskCreate(entry, "task", 512, arg, priority, &handle);  // 512 words ≈ 2KB
```

`uxTaskGetStackHighWaterMark()` 在原生 FreeRTOS 返回 word。任务预算表可以直接按 word
记录（`kTaskStackWords`）；如果预算按字节估算，用一个纯函数换算，不要为此封装任务 API：

```cpp
constexpr UBaseType_t ToStackWords(uint32_t bytes) {
	return static_cast<UBaseType_t>(
		(bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t));
}
```

队列、任务、定时器直接在 `Init()`/`Start()` 里用原生 API 创建；
定时器用 `pvTimerGetTimerID()` 携带 `this`，不需要 trampoline。

## 4. 堆上限：启动时一次性分配

小 RAM 平台不再逐对象静态创建，统一策略：

1. 先把不用 RTOS 堆的 RAM 布置完：全局/静态对象、显示 framebuffer、DMA 缓冲、
   协议栈静态缓冲、主栈；
2. 把剩余 RAM 配成 `configTOTAL_HEAP_SIZE`（heap_4 单块；RAM 不连续用 heap_5，
   启动时先 `vPortDefineHeapRegions()`）；
3. 启动时一次性创建所有任务/队列/定时器/信号量，运行期不 create/delete/free；
4. 用 `xPortGetFreeHeapSize()` / `xPortGetMinimumEverFreeHeapSize()` 验收水位。

```c
// FreeRTOSConfig.h
#define configSUPPORT_DYNAMIC_ALLOCATION  1
#define configSUPPORT_STATIC_ALLOCATION   0
#define configTOTAL_HEAP_SIZE   ((size_t)(100 * 1024))   // 剩余 RAM - 保留
#define configUSE_MALLOC_FAILED_HOOK       1
#define configCHECK_FOR_STACK_OVERFLOW     2
```

```c
// 启动阶段分配失败 = 预算算错，停下来并记录
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
```

- 动态分配时**不需要** `vApplicationGetIdleTaskMemory` / `vApplicationGetTimerTaskMemory`；
  idle 与 timer 任务的栈也从同一个堆里分配，要计入预算。
- 堆太大直接链接失败（`ucHeap` 在 `.bss`），比运行期才发现更容易定位；
- CCM/DTCM 等区域不能给 DMA，用 heap_5 单独划分前先查芯片手册。

## 5. FreeRTOSConfig.h 关键项

| 配置 | 建议值 | 原因 |
| --- | --- | --- |
| `configUSE_PREEMPTION` | `1` | 抢占式 |
| `configUSE_TIME_SLICING` | `1` | 同优先级轮转 |
| `configTICK_RATE_HZ` | `1000` | 1 ms 分辨率；需 `configUSE_16_BIT_TICKS = 0` |
| `configMAX_PRIORITIES` | `7` | 够用 |
| `configMINIMAL_STACK_SIZE` | `128` | 空闲任务栈（word） |
| `configSUPPORT_DYNAMIC_ALLOCATION` | `1` | 启动一次性分配（堆上限） |
| `configSUPPORT_STATIC_ALLOCATION` | `0` | 不做逐对象静态创建 |
| `configTOTAL_HEAP_SIZE` | 剩余 RAM - 保留 | 任务栈 + 队列 + 定时器 + 信号量 + idle/timer 任务 |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 调试期必开 |
| `configUSE_MALLOC_FAILED_HOOK` | `1` | 启动分配失败要能停下来 |
| `INCLUDE_vTaskDelayUntil` | `1` | 周期任务 |
| `INCLUDE_uxTaskGetStackHighWaterMark` | `1` | 定栈大小 |

## 6. HAL 回调 → 只投递

HAL 的 UART/EXTI 回调通常运行在 ISR 上下文：

```cpp
// g_usart1_serial 在组合根 Init() 里注册；回调里不做全局查找
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance == USART1 && g_usart1_serial != nullptr) {
		g_usart1_serial->HandleRxCpltEvent();   // 只上报，不解析
	}
}
```

驱动内部再调用 `xQueueSendFromISR` / `xSemaphoreGiveFromISR`，
最后 `portYIELD_FROM_ISR(woken)`。ISR 里不解析协议、不打印、不阻塞。

## 7. EIDE / Keil（AC6）

### EIDE

FreeRTOS 目录不要整体丢进 `srcDirs`：`portable/` 下有十几个移植和多个 heap 实现，
全部编译会撞符号。用 `virtualFolder` 显式列文件：

```yaml
files:
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/tasks.c
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/queue.c
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/list.c
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/timers.c
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F/port.c  # AC6
  - path: ../Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c       # 滚动分配
  # 只保留一个 heap_*.c：heap_1 不支持 free，heap_2 已废弃，heap_5 用于多块 RAM
```

include 路径：

```text
../Middlewares/Third_Party/FreeRTOS/Source/include
../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM4F
../Core/Inc                 # FreeRTOSConfig.h
```

- GCC 工程把 `RVDS/ARM_CM4F` 换成 `GCC/ARM_CM4F`；
- 内核按 C 编译；`app/`、`bsp/` 按 C++17 编译；
- 关闭异常/RTTI：`-fno-exceptions -fno-rtti`。

### Keil MDK

- 新建 FreeRTOS 组，加入同一份文件清单（`port.c` 选 `RVDS/ARM_CM4F`）；
- `IncludePath` 追加 Source/include 与 port 目录；
- 确认 `<uAC6>1</uAC6>`，否则 AC5 编不过 C++17；
- 内核文件 `FileType 1`，`.cpp` 文件 `FileType 8`。

## 8. 验证

```powershell
arm-none-eabi-nm build/<target>/<name>.elf | Select-String "SysTick_Handler|PendSV_Handler|SVC_Handler"
```

- 每个向量只能有一份定义；
- 启动后 `xPortGetMinimumEverFreeHeapSize` 稳定，没有运行期分配；
- 启动日志显示任务创建成功、无栈溢出；
- `uxTaskGetStackHighWaterMark` 检查栈余量；
- 条件编译分支分别构建通过。
