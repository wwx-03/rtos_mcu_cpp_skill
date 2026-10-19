# 平台契约（Platform Profile）

生成代码前必须完成本表。它的作用不是“记录 MCU 型号”，而是把**会影响代码正确性的 RTOS/芯片语义**显式化，避免 AI 用 STM32 或 vanilla FreeRTOS 的经验去猜 ESP-IDF、CMSIS-RTOS2 或其他移植。

> 硬规则：
> 1. 不确定的项必须查目标 SDK 的头文件/文档。
> 2. 查不到就问用户，或标 `TODO`，禁止猜。
> 3. 契约写入项目 `docs/platform.md`，后续改动沿用。
> 4. 平台差异只允许出现在 `port/`，不要散落在 `app/`。

## 1. 模板

```markdown
# Platform Profile

## 基本信息
- MCU / 芯片：
- 架构 / 核数：
- RTOS / 版本：
- SDK / 版本：
- 构建系统：
- Flash / RAM / 分区限制：

## 启动与调度
- 入口函数：main() / app_main() / 其他：
- 调度器是否已启动：
- 谁启动调度器：
- Application::Start() 是否允许启动调度器：否
- 单例构造时机：

## 任务与栈
- 任务创建 API：
- 栈深单位：byte / word
- 栈高水位单位：byte / word
- 静态分配是否支持：
- 空闲任务/定时器任务内存回调是否必需：
- 任务优先级范围：

## 中断与并发
- ISR 投递 API：
- ISR 中 yield API：
- 中断优先级/级别约束：
- 临界区语义：单核关中断 / 跨核 spinlock
- 双核任务亲和性要求：
- 原子操作支持：

## 回调上下文
- 厂商事件回调运行上下文：ISR / 任务 / 未知
- 定时器回调运行上下文：
- 协议栈回调是否允许阻塞：
- 是否允许在回调里 stop/free：

## 外设与总线
- I2C/SPI 驱动是否有内部总线锁：
- 事务超时 API：
- DMA/PSRAM/内存能力：
- 看门狗：

## 构建与验证
- 组件/源文件组织：
- include 约定：
- 必须的编译选项：
- 全新构建命令：
- size/分区检查命令：
- 启动 smoke test：
```

## 2. 逐项查证方法

| 契约项 | 查什么 | 常见坑 |
| --- | --- | --- |
| 入口/调度器 | 启动文件、`main.c`、`app_main` 文档 | ESP-IDF 的 `app_main` 已在调度器中；CubeMX 的 `main()` 需要自己启动 |
| 栈单位 | `task.h` 中 `xTaskCreate`/`xTaskCreateStatic` 的参数说明 | vanilla FreeRTOS 是 word；ESP-IDF 是 byte |
| 高水位单位 | `uxTaskGetStackHighWaterMark` 注释 | ESP-IDF 返回 byte，不是 word |
| 静态分配 | `configSUPPORT_STATIC_ALLOCATION` / 平台文档 | 静态工程必须提供 idle/timer 任务内存回调 |
| ISR API | port 头文件、RTOS 文档 | ISR 只能用 `...FromISR`，最后处理 woken |
| 中断优先级 | NVIC/PLIC/ESP-IDF 中断文档 | Cortex-M 的 5~15 不能直接套到 RISC-V |
| 临界区 | RTOS port、芯片手册 | 双核 `taskENTER_CRITICAL` 只关当前核 |
| 定时器上下文 | RTOS/厂商 timer 文档 | 定时器回调通常不能阻塞、不能 stop/free |
| 厂商事件上下文 | WiFi/MQTT/协议栈文档 | 事件任务里做重活会阻塞整个协议栈 |
| 构建 | SDK 示例、组件 CMake、链接脚本 | 组件依赖/分区不足会在链接或运行期才暴露 |

## 3. 示例：ESP32-C3 + ESP-IDF 6.x

```markdown
- MCU / 芯片：ESP32-C3（单核 RISC-V）
- RTOS / 版本：ESP-IDF FreeRTOS
- SDK / 版本：ESP-IDF 6.0.x
- 构建系统：idf.py / CMake / Ninja
- 入口函数：app_main()
- 调度器是否已启动：是
- 谁启动调度器：ESP-IDF 启动代码
- 任务创建 API：xTaskCreate / xTaskCreatePinnedToCore
- 栈深单位：byte
- 栈高水位单位：byte
- 静态分配：支持，但大 RAM 平台通常动态创建
- ISR 投递 API：xQueueSendFromISR / xSemaphoreGiveFromISR
- ISR yield：portYIELD_FROM_ISR(woken)
- 临界区语义：单核关中断；双核需 portMUX/原子
- 定时器回调：esp_timer 默认任务上下文（dispatch 可配置）
- 厂商事件：esp_event 默认事件任务；WiFi/MQTT/SmartConfig 回调均为任务上下文
- include 约定：`#include "freertos/FreeRTOS.h"`
- 组件依赖：`REQUIRES freertos`（IDF 6 必须显式声明）
```

## 4. 示例：STM32 + CubeMX + FreeRTOS

```markdown
- MCU / 芯片：STM32F4 / Cortex-M4F（单核）
- RTOS / 版本：FreeRTOS（CubeMX 生成）
- 入口函数：main()
- 调度器是否已启动：否
- 谁启动调度器：Application::Start() → port::StartScheduler()
- 任务创建 API：xTaskCreate / xTaskCreateStatic
- 栈深单位：word（StackType_t）
- 栈高水位单位：word
- 静态分配：小 RAM 平台建议开启
- ISR 投递 API：xQueueSendFromISR / xSemaphoreGiveFromISR
- ISR yield：portYIELD_FROM_ISR(woken)
- 临界区语义：单核关中断（NVIC 优先级分组 4）
- 定时器回调：FreeRTOS timer task
- 厂商事件：HAL UART/EXTI 回调通常运行在 ISR 上下文
- HAL 时基：改用 TIM，不要让 HAL 用 SysTick
- 中断优先级：调用 FromISR 的中断抢占优先级数值 ≥
  configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
```

## 5. 红色告警

出现下面情况时，先停下来查文档，不要继续生成：

- 看到 `usStackDepth` / `ulStackDepth`，但不知道单位；
- 看到 `app_main`，却准备调用 `vTaskStartScheduler()`；
- 看到 `taskENTER_CRITICAL`，但目标是双核；
- 在厂商事件回调里准备 `stop/close/deinit`；
- 在 ISR 里准备打印、延时、分配、拿互斥量；
- 不确定某个回调是 ISR 还是任务上下文；
- 准备用旧版本 SDK 的 API（例如 IDF 6 的 `freertos/` 前缀、`i2c_master`、托管 mqtt）。

**没有填完契约就开始写代码，视为未完成平台适配。**
