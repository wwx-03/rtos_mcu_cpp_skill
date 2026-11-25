# STM32 / CubeMX 模板使用说明

## 复制位置

```text
<project>/
├── Core/Src/main.c                  <- 参考 main_template.cpp 的启动顺序
├── Core/Inc/FreeRTOSConfig.h        <- 参考 freertos_config_notes.md
└── <app/bsp/devices/services/utility>/   <- 复制 templates/app、templates/devices
```

**没有 `port/` 目录**：`app/` 与 `bsp/` 直接 `#include "FreeRTOS.h"`、`"task.h"`、
`"queue.h"`、`"timers.h"` 并调用原生 API。

## 关键约定

- 任务栈单位是 **word**（`StackType_t`）；预算按 word 记录，或写 `ToStackWords()` 换算。
- `xTaskCreate` / `xQueueCreate` / `xTimerCreate` 在 `Init()` / `Start()` 里一次创建，
  运行期不 create/delete/free。
- `configTOTAL_HEAP_SIZE` = 剩余 RAM − 保留；heap_4 单块，RAM 不连续用 heap_5。
- `vTaskStartScheduler()` 只在 `main()` 里调用一次；返回说明堆不足/任务创建失败。
- HAL 时基改用 TIM，SysTick/PendSV/SVC 交给 FreeRTOS。
- 调用 `...FromISR` 的中断，其 NVIC 抢占优先级数值要满足
  `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`。
- HAL UART/EXTI 回调通常运行在 ISR 上下文：只投递，不解析/不打印/不阻塞。
- 定时器回调用 `pvTimerGetTimerID()` 取 `this`，运行在 timer task，只投递。

## 验证

- `arm-none-eabi-nm` 确认 SysTick/PendSV/SVC 只有一份定义；
- 链接通过说明 `configTOTAL_HEAP_SIZE` 放得下；
- 启动日志无栈溢出、无 HardFault；
- 启动后 `xPortGetMinimumEverFreeHeapSize()` 稳定；
- 条件编译分支分别构建通过。