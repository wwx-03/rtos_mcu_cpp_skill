# 平台契约（Platform Profile）

生成代码前必须完成本表。它的作用不是“记录 MCU 型号”，而是把**会影响代码正确性的 RTOS/芯片语义**显式化，避免 AI 用 STM32 或 vanilla FreeRTOS 的经验去猜 ESP-IDF、CMSIS-RTOS2 或其他移植。

> 硬规则：
> 1. 不确定的项必须查目标 SDK 的头文件/文档。
> 2. 查不到就问用户，或标 `TODO`，禁止猜。
> 3. 契约写入项目 `docs/platform.md`，后续改动沿用。
> 4. 平台差异只允许出现在 `port/`，不要散落在 `app/`。
> 5. 架构模式（Layered / DIP / Hybrid）必须选定并写入契约，且由构建系统强制；
>    只在文档里写分层不算完成。
> 6. Hybrid 的“逐模块依赖”不能只靠组件级 `REQUIRES` 强制；必须让实现层不暴露
>    公共头，或把模块拆成独立组件。例外写入模块矩阵。

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

## 架构模式
- 模式：Layered / DIP / Hybrid
- 选择理由（变体/测试/复杂度/团队/资源）：
- app 可见的抽象：bsp / devices+services / 混合
- 模块边界：哪些模块走接口，哪些用具体类
- Board 形态：具体类 / 抽象类 + 具体板类名
- 具体板实现文件（DIP/Hybrid）：`bsp/<board>_board.cpp`（类只在 .cpp 内定义）
- 参与编译的板级 .cpp 数量：1（多个 → 链接期重复符号）
- 构建约束：app 不允许 include 的层；CMake（或等价机制）如何强制
- 实现层是否暴露公共头：是 / 否；否 → 头文件位置与 `PRIV_INCLUDE_DIRS`
- 模块矩阵文件：`docs/module-matrix.md`
- Hybrid 例外：哪些简单模块允许 app 直接 include bsp

## 启动与调度
- 入口函数：main() / app_main() / 其他：
- 调度器是否已启动：
- 谁启动调度器：
- Application::Start() 是否允许启动调度器：否
- 资源创建 / 任务创建：Init() / Start()
- 单例构造时机：

## 任务与栈
- 任务创建 API：
- 栈深单位：byte / word
- 栈高水位单位：byte / word
- 静态分配是否支持：
- 空闲任务/定时器任务内存回调是否必需：
- 任务优先级范围：
- 任务预算表（任务数/栈/周期/阻塞点）：
- `port` 是否提供 timer / 栈高水位 / heap 诊断：

## 分配与 C++
- 运行期是否允许 new/malloc：
- `std::function` / `std::string` / STL 容器策略：
- 异常/RTTI：开启 / 关闭
- 分配失败策略：abort / 降级 / 重试
- 静态分配扩展：类型、对齐、栈单位

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
- 主机侧测试入口 / mock 策略：
- 产品级检查（OTA/低功耗/看门狗/NVS/安全）：见 `references/product-architecture.md`
```

> 模式选择判据、目录、Board 形态与构建约束见 `references/architecture.md`。

## 2. 逐项查证方法

| 契约项 | 查什么 | 常见坑 |
| --- | --- | --- |
| 架构模式 | 板卡/器件变体、测试需求、团队规模、ROM/RAM | ROM/RAM 不是主判据；写了模式但构建没约束等于没做 |
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

## 3. 红色告警

出现下面情况时，先停下来查文档，不要继续生成：

- 看到 `usStackDepth` / `ulStackDepth`，但不知道单位；
- 看到 `app_main`，却准备调用 `vTaskStartScheduler()`；
- 看到 `taskENTER_CRITICAL`，但目标是双核；
- 在厂商事件回调里准备 `stop/close/deinit`；
- 在 ISR 里准备打印、延时、分配、拿互斥量；
- 不确定某个回调是 ISR 还是任务上下文；
- app 同时 include bsp 和 devices，但没有逐模块规则；
- Board.hpp 暴露具体驱动类型，app 直接依赖具体 BSP；
- 只在文档里写 Layered/DIP，但构建系统仍允许任意 include；
- DIP 模式下 app 仍然 include `Board.hpp` 或具体 BSP 头；
- 抽象 `Board` 上出现喂狗/板号/引脚常量等板级专用内容；
- 具体板类仍叫 `Board`，或为具体板写了 `.hpp` 让 app include；
- 准备用旧版本 SDK 的 API（例如 IDF 6 的 `freertos/` 前缀、`i2c_master`、托管 mqtt）。

**没有填完契约就开始写代码，视为未完成平台适配。**
