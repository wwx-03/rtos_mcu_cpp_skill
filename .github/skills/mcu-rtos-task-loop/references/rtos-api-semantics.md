# RTOS API 语义对照（FreeRTOS / ESP-IDF / CMSIS-RTOS2）

本文件记录**会改变代码正确性**的 RTOS 语义差异。它比“具体 MCU 型号”更稳定，但仍会随
SDK 版本变化；使用前请对照目标 SDK 的头文件确认。

**代码直接用所选用 RTOS 的原生 API，不再包一层 `port/` 转接。**
下表是查证清单，不是封装接口。

## 1. 一页对照表

| 项目 | 原生 FreeRTOS（Cortex-M 等） | ESP-IDF | CMSIS-RTOS2 |
| --- | --- | --- | --- |
| 入口 | 用户 `main()` | `app_main()` | 用户 `main()` |
| 调度器 | `vTaskStartScheduler()` 一次 | 启动代码已启动；不要再调用 | `osKernelStart()` 一次 |
| 任务创建 | `xTaskCreate` | `xTaskCreate` / `xTaskCreatePinnedToCore` | `osThreadNew` |
| 栈深单位 | **word**（`StackType_t`） | **byte** | `osThreadAttr_t.stack_size` 为 byte |
| 栈高水位单位 | word | **byte** | `osThreadGetStackSpace` 返回 byte（按实现） |
| ISR 入队 | `xQueueSendFromISR(..., &woken)` | 同左 | `osMessageQueuePut(..., timeout=0)` |
| ISR 释放信号量 | `xSemaphoreGiveFromISR` | 同左 | `osSemaphoreRelease` |
| ISR yield | `portYIELD_FROM_ISR(woken)` | 同左 | `osThreadFlagsSet` 等 |
| 临界区 | `taskENTER_CRITICAL()` | 需要 `portMUX_TYPE`；双核需 spinlock | `osKernelLock` 或平台原语 |
| 软件定时器回调 | timer task | FreeRTOS timer task 或 `esp_timer` | `osTimer` 回调（按实现） |
| Tick | `configTICK_RATE_HZ` | 默认 100 Hz，可配置 | `osKernelGetTickFreq()` |

> 千万不要把这张表当成“永久真理”。写代码前打开目标 SDK 的头文件，
> 确认参数单位与回调上下文。

## 2. 栈单位：最容易出错的一项

### 原生 FreeRTOS

```c
// 栈深单位是 word（StackType_t）
xTaskCreate(entry, "task", 512, arg, priority, &handle);
// 512 words ≈ 2KB（Cortex-M）
```

### ESP-IDF

```c
// 栈深单位是 byte，不是 word
xTaskCreate(entry, "task", 4096, arg, priority, &handle);
// 4096 bytes = 4KB
```

ESP-IDF 的 `uxTaskGetStackHighWaterMark()` 也返回 **byte**：

```c
UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(handle);
```

**处理方式**：任务预算表按平台单位记录，常量名与单位一致
（ESP-IDF 用 `kTaskStackBytes`，原生 FreeRTOS 用 `kTaskStackWords`）。
需要在两种单位间换算时，写一个纯函数放 `utility/`：

```cpp
constexpr UBaseType_t ToStackWords(uint32_t bytes) {
	return static_cast<UBaseType_t>(
		(bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t));
}
```

不要为此再造一层 `CreateTask` 封装。

## 3. 启动与调度器

| 平台 | 入口 | 调度器 |
| --- | --- | --- |
| 原生 FreeRTOS / CubeMX | `main()` | `vTaskStartScheduler()` 一次 |
| ESP-IDF | `app_main()` | 启动代码已启动；不要再调用 |
| CMSIS-RTOS2 | `main()` | `osKernelStart()` 一次 |

`Application::Init()` 建队列/信号量/定时器；`Application::Start()` 只创建任务/启动 timer。
调度器由平台入口调用一次；完整入口代码见 `esp-idf.md` / `stm32-cubemx.md`。

## 4. ISR 安全 API

| 操作 | FreeRTOS | CMSIS-RTOS2 |
| --- | --- | --- |
| 队列发送 | `xQueueSendFromISR(q, &item, &woken)` | `osMessageQueuePut(q, &item, 0, 0)` |
| 二值信号量 | `xSemaphoreGiveFromISR(sem, &woken)` | `osSemaphoreRelease(sem)` |
| 任务通知 | `vTaskNotifyGiveFromISR(task, &woken)` | `osThreadFlagsSet(task, flags)` |
| yield | `portYIELD_FROM_ISR(woken)` | 由内核在返回时处理 |

规则：

- ISR 里只能用 `...FromISR` / timeout = 0 的 API；
- `woken` 必须是 `BaseType_t` 局部变量，初始化为 `pdFALSE`，末尾处理；
- 不要在 ISR 里打印、延时、分配、拿互斥量、写 Flash；
- 需要通知任务的中断，其优先级必须满足 RTOS 的 ISR 安全约束；
- 中断优先级规则是芯片/移植相关的，不要跨平台套用。

## 5. 临界区与多核

### 单核 Cortex-M

```c
taskENTER_CRITICAL();
shared_counter++;
taskEXIT_CRITICAL();
```

只关闭当前核中断，适合极短、不与 RTOS API 交叉的代码。

### ESP-IDF（6.x）

ESP-IDF 6 的 `taskENTER_CRITICAL()` 需要 `portMUX_TYPE` 参数，单核也不例外：

```c
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
taskENTER_CRITICAL(&lock);
shared_counter++;
taskEXIT_CRITICAL(&lock);
```

`taskENTER_CRITICAL()` 只关当前核中断，**不能保护另一个核**。跨核共享数据需要：

```c
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

portENTER_CRITICAL(&lock);
shared_counter++;
portEXIT_CRITICAL(&lock);
```

或者使用 `std::atomic` / 互斥量。互斥量仍然只能在任务里使用。

## 6. 定时器与回调上下文

| 类型 | 运行上下文 | 能做什么 |
| --- | --- | --- |
| FreeRTOS software timer | timer task | 只投递；不阻塞 |
| `esp_timer`（dispatch = task） | esp_timer task | 只投递；不阻塞 |
| `esp_timer`（dispatch = ISR） | ISR | 只做 ISR 安全操作 |
| 厂商协议栈 timer | 按 SDK | 先查文档 |
| 厂商事件回调 | 可能是 ISR，也可能是任务 | 先查文档；不 stop/free |

**需要在回调里停止/释放资源时**：把动作投递到普通任务，或用一次性 timer 延迟到任务上下文。

FreeRTOS 定时器直接用 `pvTimerGetTimerID()` 携带 `this`，不需要额外的 trampoline 上下文：

```cpp
TimerHandle_t timer = xTimerCreate("button", pdMS_TO_TICKS(20), pdTRUE, this,
	&Button::OnTimer);
```

## 7. FreeRTOS ↔ CMSIS-RTOS2 映射

| FreeRTOS | CMSIS-RTOS2 |
| --- | --- |
| `xTaskCreate` | `osThreadNew` + `osThreadAttr_t` |
| `vTaskDelete` | `osThreadTerminate` |
| `xQueueCreate` / `xQueueSend` / `xQueueReceive` | `osMessageQueueNew` / `osMessageQueuePut` / `osMessageQueueGet` |
| `xQueueSendFromISR` | `osMessageQueuePut(..., timeout=0)`（ISR 里） |
| `xSemaphoreCreateBinary` / `xSemaphoreGive` | `osSemaphoreNew(1, 0, NULL)` / `osSemaphoreRelease` |
| `xSemaphoreCreateMutex` | `osMutexNew` |
| `xTaskNotifyGive` / `ulTaskNotifyTake` | `osThreadFlagsSet` / `osThreadFlagsWait` |
| `xTimerCreate` | `osTimerNew` |
| `vTaskDelay` / `vTaskDelayUntil` | `osDelay` / `osDelayUntil` |
| `uxTaskGetStackHighWaterMark` | `osThreadGetStackSpace` |
| `xPortGetFreeHeapSize` / `xPortGetMinimumEverFreeHeapSize` | 取决于实现（不一定提供） |

CMSIS-RTOS2 只是 API 封装，**中断优先级、堆配置、栈语义仍由底层 RTOS/移植决定**。

## 8. 已知易错点

- 把 ESP-IDF 的栈大小当 word → 实际只分配 1/4；
- 在 `app_main` 里再次 `vTaskStartScheduler()`；
- 在厂商事件回调里 `esp_smartconfig_stop()` / `esp_wifi_stop()` / `esp_mqtt_client_stop()`；
- 在 ISR 里调用 `xQueueSend` 而不是 `xQueueSendFromISR`；
- 在双核上用 `taskENTER_CRITICAL` 保护跨核数据；
- 在 FreeRTOS timer 回调里等队列或阻塞；
- 把 `uxTaskGetStackHighWaterMark` 的返回值当 word；
- 用 `vTaskDelay` 做周期任务，导致累积漂移；
- 在 ISR 里打印或调用 `snprintf`；
- 为了“平台无关”再包一层 `port::`，结果两套语义都要维护。