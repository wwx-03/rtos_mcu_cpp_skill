# FreeRTOS 移植层语义对照

本文件记录**会改变代码正确性**的 RTOS 移植差异。它比“具体 MCU 型号”更稳定，但仍会随 SDK 版本变化；使用前请对照目标 SDK 的头文件确认。

## 1. 一页对照表

| 项目 | 原生 FreeRTOS（Cortex-M 等） | ESP-IDF | CMSIS-RTOS2 |
| --- | --- | --- | --- |
| 入口 | 用户 `main()` | `app_main()` | 用户 `main()` |
| 调度器 | `vTaskStartScheduler()` 一次 | 启动代码已启动；不要再调用 | `osKernelStart()` 一次 |
| 任务创建 | `xTaskCreate` / `xTaskCreateStatic` | 同左 + `xTaskCreatePinnedToCore` | `osThreadNew` |
| 栈深单位 | **word**（`StackType_t`） | **byte** | `osThreadAttr_t.stack_size` 为 byte |
| 栈高水位单位 | word | **byte** | `osThreadGetStackSpace` 返回 byte（按实现） |
| 静态分配 | 由 `configSUPPORT_STATIC_ALLOCATION` 决定 | 支持，常用动态 | 通过 `attr.cb_mem/stack_mem` |
| ISR 入队 | `xQueueSendFromISR(..., &woken)` | 同左 | `osMessageQueuePut(..., timeout=0)` |
| ISR 释放信号量 | `xSemaphoreGiveFromISR` | 同左 | `osSemaphoreRelease` |
| ISR yield | `portYIELD_FROM_ISR(woken)` | 同左 | `osThreadFlagsSet` 等 |
| 临界区 | `taskENTER_CRITICAL`（单核） | 单核有效；双核需 `portMUX_TYPE` | `osKernelLock` 或平台原语 |
| 软件定时器回调 | timer task | `esp_timer` 任务/ISR（看 dispatch） | `osTimer` 回调（按实现） |
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

`port::TaskOptions.stack_bytes` 统一用字节，由平台实现换算：

```cpp
// 原生 FreeRTOS
xTaskCreate(entry, name, stack_bytes / sizeof(StackType_t), arg, priority, &handle);

// ESP-IDF
xTaskCreate(entry, name, stack_bytes, arg, priority, &handle);
```

## 3. 启动与调度器

### 原生 FreeRTOS / CubeMX

```c
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    // ... 创建任务 ...
    vTaskStartScheduler();   // 只调用一次
    for (;;) {
    }
}
```

`Application::Start()` 可以负责创建任务并启动调度器，但必须保证只调用一次。

### ESP-IDF

```cpp
extern "C" void app_main(void) {
    // app_main 本身已经运行在调度器创建的 main task 中
    Board::GetInstance().Init();
    Application::GetInstance().Start();   // 只建任务
    // 不要调用 vTaskStartScheduler()
}
```

### CMSIS-RTOS2

```c
int main(void) {
    osKernelInitialize();
    // 创建线程...
    osKernelStart();   // 只调用一次
    for (;;) {
    }
}
```

## 4. ISR 安全 API

| 操作 | FreeRTOS | CMSIS-RTOS2 |
| --- | --- | --- |
| 队列发送 | `xQueueSendFromISR(q, &item, &woken)` | `osMessageQueuePut(q, &item, 0, 0)` |
| 二值信号量 | `xSemaphoreGiveFromISR(sem, &woken)` | `osSemaphoreRelease(sem)` |
| 任务通知 | `vTaskNotifyGiveFromISR(handle, &woken)` | `osThreadFlagsSet(handle, flags)` |
| yield | `portYIELD_FROM_ISR(woken)` | 由内核在返回时处理 |

规则：

- ISR 里只能用 `...FromISR` / timeout = 0 的 API；
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

## 7. CMSIS-RTOS2 映射

| 本技能（FreeRTOS 原生） | CMSIS-RTOS2 |
| --- | --- |
| `xTaskCreateStatic` | `osThreadNew` + `osThreadAttr_t`（`.stack_mem` / `.cb_mem`） |
| `xQueueCreateStatic` | `osMessageQueueNew` + `.mq_mem` / `.mq_size` |
| `xQueueSend` / `xQueueSendFromISR` | `osMessageQueuePut(..., timeout)`（ISR 里 timeout 必须 0） |
| `xQueueReceive` | `osMessageQueueGet` |
| `xSemaphoreCreateBinaryStatic` | `osSemaphoreNew(1, 0, &attr)` |
| `xSemaphoreCreateMutexStatic` | `osMutexNew` + `.cb_mem` |
| `xSemaphoreGiveFromISR` | `osSemaphoreRelease`（ISR 安全） |
| `xTaskNotifyFromISR` / `ulTaskNotifyTake` | `osThreadFlagsSet` / `osThreadFlagsWait` |
| `xTimerCreateStatic` | `osTimerNew` + `.cb_mem` |
| `vTaskDelay` / `vTaskDelayUntil` | `osDelay` / `osDelayUntil` |
| `taskENTER_CRITICAL` | 短临界区仍用平台原语；长临界区用 `osKernelLock` |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 同一份 FreeRTOSConfig 生效 |

CMSIS-RTOS2 只是 API 封装，**中断优先级、分配策略、栈语义仍由底层 RTOS/移植决定**。

## 8. 已知易错点

- 把 ESP-IDF 的栈大小当 word → 实际只分配 1/4；
- 在 `app_main` 里再次 `vTaskStartScheduler()`；
- 在厂商事件回调里 `esp_smartconfig_stop()` / `esp_wifi_stop()` / `esp_mqtt_client_stop()`；
- 在 ISR 里调用 `xQueueSend` 而不是 `xQueueSendFromISR`；
- 在双核上用 `taskENTER_CRITICAL` 保护跨核数据；
- 在 FreeRTOS timer 回调里等队列或阻塞；
- 把 `uxTaskGetStackHighWaterMark` 的返回值当 word；
- 用 `vTaskDelay` 做周期任务，导致累积漂移；
- 在 ISR 里打印或调用 `snprintf`。
