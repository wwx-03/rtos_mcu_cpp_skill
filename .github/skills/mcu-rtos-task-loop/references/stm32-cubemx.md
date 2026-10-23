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

`port::TaskOptions.stack_bytes` 统一用字节，STM32 port 实现里换算：

```cpp
xTaskCreate(entry, name, stack_bytes / sizeof(StackType_t), arg, priority, &handle);
```

`uxTaskGetStackHighWaterMark()` 在原生 FreeRTOS 返回 word；`port::GetStackHighWaterMark`
会换算成字节，和 ESP-IDF 的语义保持一致。

`templates/stm32/port/rtos_port.cpp` 还提供 `CreateTimer/Start/Stop/Reset/DeleteTimer`
以及堆诊断。timer 回调运行在 timer task，只能投递。
静态 task/queue 用 `port::StaticTask` / `port::StaticQueue`（见 `templates/port/rtos_port.hpp`），
完整静态/动态示例见 `templates/app/`。

## 4. 静态分配

小 RAM 平台建议：

```c
#define configSUPPORT_STATIC_ALLOCATION   1
#define configSUPPORT_DYNAMIC_ALLOCATION  0
```

> `templates/stm32/port/rtos_port.cpp` 同时提供动态 task/queue/timer 和静态
> task/queue/timer（`port::StaticTask` / `StaticQueue` / `StaticTimer`）。
> 设置 `configSUPPORT_DYNAMIC_ALLOCATION = 0` 后，动态 `CreateTask` / `CreateQueue` /
> `CreateTimer` / `CreateBinarySemaphore` / `CreateMutex` 需要按 SDK 裁剪或替换；
> 静态信号量仍需 `xSemaphoreCreateStatic`。
> 静态任务模板见 `templates/app/`。

此时必须提供空闲任务/定时器任务内存：

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

## 5. FreeRTOSConfig.h 关键项

| 配置 | 建议值 | 原因 |
| --- | --- | --- |
| `configUSE_PREEMPTION` | `1` | 抢占式 |
| `configUSE_TIME_SLICING` | `1` | 同优先级轮转 |
| `configTICK_RATE_HZ` | `1000` | 1 ms 分辨率；需 `configUSE_16_BIT_TICKS = 0` |
| `configMAX_PRIORITIES` | `7` | 够用 |
| `configMINIMAL_STACK_SIZE` | `128` | 空闲任务栈（word） |
| `configSUPPORT_STATIC_ALLOCATION` | 小 RAM：`1` | 静态创建 |
| `configSUPPORT_DYNAMIC_ALLOCATION` | 小 RAM：`0` | 与策略一致 |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 调试期必开 |
| `configUSE_MALLOC_FAILED_HOOK` | 动态分配时 `1` | 堆失败要能停下来 |
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

驱动内部再调用 `port::QueueSendFromIsr` / `port::SemaphoreGiveFromIsr`，
最后 `port::YieldFromIsr(woken)`。ISR 里不解析协议、不打印、不阻塞。

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
  # 静态分配时不要加 heap_4.c
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
- 静态分配工程里没有引入 `malloc`/`free`；
- 启动日志显示任务创建成功、无栈溢出；
- `uxTaskGetStackHighWaterMark` 检查栈余量；
- 条件编译分支分别构建通过。
