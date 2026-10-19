# mcu-rtos-task-loop

面向 **RTOS MCU 工程**（FreeRTOS / CMSIS-RTOS2，Cortex-M / STM32 / ESP32 / 嵌入式 C++）的 Copilot Agent Skill。

它不教你"怎么调用 `xQueueSend`"，而是约束工程**结构**：中断、事件循环、APP 模块、任务与队列的职责边界，以及 CubeMX / EIDE / Keil 里最容易配错的那几项。目标是让生成的代码符合一套固定的多任务模式，而不是每次重新发明。

## 核心约束

1. **中断只做三件事，并且只用 FromISR API**
   清标志、搬数据、投递（`xQueueSendFromISR` / `xSemaphoreGiveFromISR` / `xTaskNotifyFromISR`），最后处理 `xHigherPriorityTaskWoken`。ISR 里不打印、不延时、不阻塞、不拿互斥量。
2. **资源只在初始化阶段创建一次**
   任务 / 队列 / 信号量 / 定时器在 `Start()` 路径上创建，运行期不 create/delete。
   小 RAM 平台用 `xTaskCreateStatic` / `xQueueCreateStatic`，并关闭动态分配；ESP32 等大 RAM 平台可在 init 阶段动态创建，但必须检查返回值。
3. **共享数据只用 RTOS 原语**
   任务间传数据用队列；保护资源用互斥量（只能在任务里获取）；只有极短的临界区才用 `taskENTER_CRITICAL`。

在此之上还有两条组织规则：

- `Application` 持有主事件队列，用 `MainEventLoop()` 统一派发**中断事件与跨模块事件**；
- **每个 APP 类自己持有队列和任务**，对外只暴露异步 API（`display->DrawPicture()` 只投递事件后立刻返回），依赖在构造时由 `Board` 注入。

## 覆盖内容

| 主题 | 说明 |
| --- | --- |
| 工程骨架与分层 | `app → bsp → devices → utility → port → CMSIS` 的依赖方向；`devices/` 不 include FreeRTOS 与厂商头 |
| Application 与 MainEventLoop | 构造 / `Start()` / `MainEventLoop()` 的职责划分，事件派发与 `HandleStart()` 时机 |
| APP 类设计 | 构造注入依赖、静态队列与任务、异步 API、槽位池传大报文 |
| 中断 → 任务链路 | FromISR 写法、`portYIELD_FROM_ISR`、中断优先级必须 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` |
| 分配策略 | 静态 vs 动态的取舍，`configSUPPORT_STATIC_ALLOCATION` 与空闲任务/定时器任务回调 |
| 任务设计 | 一 APP 一任务、优先级、栈大小（word 单位）、`vTaskDelayUntil` 周期任务 |
| 同步原语 | 队列 / 信号量 / 互斥量 / 任务通知 / 事件组的选择与边界 |
| 软件定时器 | 回调只投递，重活交给任务 |
| HAL 与 FreeRTOS 冲突 | HAL 时基改用 TIM；SysTick / PendSV / SVC 只保留 FreeRTOS 一份 |
| 工程配置 | EIDE 与 Keil MDK（AC6）的 FreeRTOS 文件清单、include 路径、`-fno-exceptions -fno-rtti` |
| 反模式清单 | 20+ 条"错误写法 → 正确做法"对照，以及提交前的检查清单 |
| CMSIS-RTOS2 | FreeRTOS 原生 API 与 `cmsis_os2.h` 的逐项对照表 |

## 目录结构

```text
.
└── .github/
    └── skills/
        └── mcu-rtos-task-loop/
            └── SKILL.md    # 技能正文（约 1000 行）
```

## 安装

### 项目级（推荐）

把整个技能目录复制到目标工程根目录下的 `.github/skills/`：

```text
<your-project>/
└── .github/
    └── skills/
        └── mcu-rtos-task-loop/
            └── SKILL.md
```

### 个人级

放到用户目录下的 Copilot skills 目录，对所有工程生效：

```text
~/.copilot/skills/mcu-rtos-task-loop/SKILL.md
```

克隆后也可以直接把仓库根目录当作技能的存放位置，只保留 `.github/skills/mcu-rtos-task-loop/` 一并拷贝即可。

## 使用

在 VS Code 的 Copilot Chat 中描述要做的事即可，例如：

```text
给这个 FreeRTOS 工程加一个网络模块，带异步连接与发送
```

```text
把这个裸机工程的按键中断改成 中断 → 队列 → 任务 的模式
```

```text
帮我配置 EIDE / Keil 里的 FreeRTOS 文件和中断优先级
```

技能会根据 `description` 自动匹配；也可以显式点名 `mcu-rtos-task-loop`。`argument-hint` 提示了推荐的输入格式：**要新增的 APP 模块 / 任务 / 数据流**。

## 适用与不适用

适用：

- 新建 RTOS 工程，或给现有工程加 RTOS；
- 新增 APP 模块、任务、队列、信号量、软件定时器、串口协议；
- 审查现有工程的 ISR、事件循环、任务与内存分配是否符合上述模式。

不适用：

- 纯裸机（前后台）工程的分层与驱动写法——可参考 `non-os-mcu-cpp-utility` 一类非 RTOS 约定；
- 与 RTOS 无关的算法、协议或 UI 实现细节。

## 验证

改完工程后按 `SKILL.md` 的**检查清单**逐条核对，关键几条：

- [ ] 会通知任务的中断，抢占优先级 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`
- [ ] `Application` 构造函数只建队列 / 注册回调，任务在 `HandleStart()` 创建
- [ ] 每个 APP 类自己持有队列与任务，公有 API 都是"投递后返回"
- [ ] 运行期没有 `new` / `malloc` / STL 容器反复增删
- [ ] SysTick / PendSV / SVC 只有一个定义，HAL 时基来自 TIM
- [ ] 用工程实际工具链完整构建通过（AC6 或 GCC，C++17）

## 许可

本仓库未声明开源许可证。如需对外发布，请补充合适的 LICENSE。
