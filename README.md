# mcu-rtos-task-loop

面向 **RTOS MCU 工程**（FreeRTOS / CMSIS-RTOS2，Cortex-M / STM32 / ESP32 / 嵌入式 C++）的 Copilot Agent Skill。

它的核心是**平台无关的 RTOS 设计约束**：中断、事件循环、APP 模块、任务与队列的职责边界。
MCU 厂商、SDK 版本、栈单位、ISR 规则等会随平台变化的内容不写死在核心里，而是通过
**「平台契约」+ `port/` 适配层 + references/templates** 解决。

目标是让 AI：

1. 先识别目标 MCU/RTOS/SDK，确认平台语义；
2. 生成统一的 `port/` 实现；
3. 再按同一套结构生成 `app / bsp / devices / utility`。

而不是每次重新发明任务划分，或者把 STM32 的经验直接套到 ESP-IDF 上。

## 核心约束

1. **中断只做三件事，并且只走 ISR 安全 API**
   清标志、搬数据、投递（`port::QueueSendFromIsr` / `port::SemaphoreGiveFromIsr`），
   最后处理 woken 并调用 `port::YieldFromIsr(woken)`。
   ISR 里不打印、不延时、不阻塞、不拿互斥量、不分配内存。
2. **资源只在初始化阶段创建一次**
   任务 / 队列 / 信号量 / 定时器在 `Start()` 路径创建，运行期不 create/delete。
   小 RAM 平台优先静态分配；大 RAM 平台可在 init 阶段动态创建，但必须检查返回值。
3. **共享数据只用 RTOS 原语**
   任务间传数据用队列；保护资源用互斥量（只能在任务里获取）；
   只有极短的临界区才用 `port::EnterCritical()`，且要确认单核/双核语义。

组织规则：

- `Application` 持有主事件队列，用 `MainEventLoop()` 统一派发**中断事件与跨模块事件**；
- **每个 APP 类自己持有队列和任务**，对外只暴露异步 API；
- 依赖在构造时由 `Board` 注入；
- **`Application::Start()` 只创建任务，不启动调度器**；启动调度器是平台入口的职责。

## 平台契约（关键新增）

生成代码前必须先填写 `references/platform-profile.md`，至少确认：

- 入口函数与调度器是否已启动；
- `xTaskCreate` / `xTaskCreateStatic` 的栈单位（byte / word）；
- 栈高水位返回值单位；
- 静态/动态分配支持；
- ISR 投递 API 与中断优先级约束；
- 临界区是否跨核；
- 定时器 / 厂商事件回调的上下文；
- 构建系统与分区限制。

> 不确定就查 SDK 头文件/文档，不许猜。

## 目录结构

```text
.github/skills/mcu-rtos-task-loop/
├── SKILL.md                         # 平台无关核心
├── references/
│   ├── platform-profile.md          # 平台契约模板与查证方法
│   ├── freertos-port-semantics.md   # FreeRTOS / ESP-IDF / CMSIS 语义差异
│   ├── esp-idf.md                   # ESP-IDF 启动、组件、IDF 6 迁移
│   ├── stm32-cubemx.md              # STM32/CubeMX、HAL、NVIC、EIDE/Keil
│   └── callback-contexts.md         # ISR / 厂商事件 / 定时器 / 任务上下文
├── templates/
│   ├── port/rtos_port.hpp           # 统一 port 接口（stack_bytes 等）
│   ├── esp-idf/                     # port 实现、app_main、CMake、Kconfig、分区
│   └── stm32/                       # 原生 FreeRTOS port、启动模板、配置说明
├── checklists/bring-up.md           # 硬件与固件 bring-up 检查清单
└── examples/sensor-to-cloud/        # 采集→滤波→显示→MQTT 案例
```

## 安装

### 项目级（推荐）

把整个技能目录复制到目标工程根目录下的 `.github/skills/`：

```text
<your-project>/
└── .github/
    └── skills/
        └── mcu-rtos-task-loop/
            ├── SKILL.md
            ├── references/
            ├── templates/
            ├── checklists/
            └── examples/
```

### 个人级

放到用户目录下的 Copilot skills 目录：

```text
~/.copilot/skills/mcu-rtos-task-loop/
```

`references/`、`templates/`、`checklists/` 要和 `SKILL.md` 一起复制；只复制 `SKILL.md` 会丢失平台契约与模板。

## 使用

在 VS Code 的 Copilot Chat 中描述要做的事，**同时给出目标平台**：

```text
ESP32-C3 + ESP-IDF 6.0，加一个 MQTT 上报模块，带异步连接与发送
```

```text
STM32F407 + FreeRTOS，把这个裸机工程改成 中断 → 队列 → 任务 的模式
```

```text
帮我审查这个工程的 ISR、事件循环、任务和内存分配，并把平台契约补全
```

推荐流程：

1. AI 识别 MCU/RTOS/SDK，填写 `references/platform-profile.md`；
2. AI 先实现 `templates/` 里的 `port/`；
3. 再生成 `app / bsp / devices / utility`；
4. 按 `checklists/bring-up.md` 做语义与构建检查。

## 覆盖内容

| 主题 | 位置 |
| --- | --- |
| 分层、依赖方向、Board 注入 | `SKILL.md` |
| Application / MainEventLoop / 启动时序 | `SKILL.md` |
| APP 异步 API、队列与载荷策略 | `SKILL.md` |
| ISR → 任务链路、回调上下文 | `SKILL.md` + `references/callback-contexts.md` |
| 任务、栈、同步原语、临界区、共享总线 | `SKILL.md` |
| 失败降级、可观测性、时间、连接状态机 | `SKILL.md` |
| 平台契约与查证方法 | `references/platform-profile.md` |
| FreeRTOS / ESP-IDF / CMSIS 语义差异 | `references/freertos-port-semantics.md` |
| ESP-IDF 6 启动、组件、API 迁移 | `references/esp-idf.md` |
| STM32/CubeMX、HAL、NVIC、EIDE/Keil | `references/stm32-cubemx.md` |
| 代码模板 | `templates/` |
| Bring-up 检查 | `checklists/bring-up.md` |
| 完整案例 | `examples/sensor-to-cloud/` |

## 验证

改完工程后按 `checklists/bring-up.md` 逐条核对，关键几条：

- [ ] 平台契约已填写，入口/调度器/栈单位/中断语义已确认
- [ ] `Application::Start()` 没有启动调度器
- [ ] 栈大小按平台单位设置，并通过高水位验证
- [ ] ISR 只投递并处理 woken；厂商事件回调里没有 stop/free/阻塞
- [ ] APP 公有 API 都是“投递后返回”
- [ ] 连接/配网/重连有唯一 owner，离线策略明确
- [ ] 初始化失败有降级/停机策略，`ready_` 标志有效
- [ ] 无 sdkconfig 的全新构建、条件编译分支分别构建通过
- [ ] `size`/分区余量、启动日志 smoke test 通过

## 平台适配说明

本技能不绑定具体 MCU。以下内容属于**平台语义**，必须在生成代码前确认，不能凭记忆：

- 调度器由谁启动、是否已启动；
- 任务栈与高水位的单位；
- ISR API 与中断优先级规则；
- 临界区是否跨核；
- 定时器/厂商事件回调上下文；
- 构建系统、组件依赖、分区/内存限制。

平台细节会随 SDK 版本变化。`references/` 中的内容是参考，不是永久真理；使用前请对照目标 SDK 的头文件与迁移指南。

## 许可

本仓库未声明开源许可证。如需对外发布，请补充合适的 LICENSE。
