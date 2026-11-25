# mcu-rtos-task-loop

面向 **RTOS MCU 工程**（FreeRTOS / CMSIS-RTOS2，Cortex-M / STM32 / ESP32 / 嵌入式 C++）的 Copilot Agent Skill。

核心是三条：**先判层，再命名，再写并发。**

- 判层：`devices`（器件）/ `services`（平台能力）/ `bsp`（实现）/ `app`（业务）/
  `utility`（纯逻辑），默认 **Hybrid**；**没有 `port/` 层**；
- 命名：目录、文件、类、方法、句柄、事件、生命周期都有唯一标准，
  改造现有工程时新代码按标准走，不沿用错误命名；
- 并发：中断只投递、`Application` 用 `MainEventLoop()` 派发、APP 只暴露异步 API、
  依赖构造注入、**直接用所选用 RTOS 的原生 API**。

平台差异（栈单位、ISR 规则、回调上下文、构建系统、RTOS 堆上限）通过
**「平台契约」+ `references/`** 解决，不写死在核心里，也不再包一层 RTOS 转接。

分配策略固定为：**先把不用 RTOS 堆的 RAM 布置完，再把剩余 RAM 配成 RTOS 堆上限，
启动时一次性创建全部任务/队列/定时器/信号量，运行期不再申请、不再释放。**

## 目录结构

```text
.github/skills/mcu-rtos-task-loop/
├── SKILL.md                         # 平台无关核心（不变量 / 流程 / 命名 / 输出契约）
├── references/
│   ├── layers-and-naming.md         # 分层判据、模块归类、命名与风格标准
│   ├── project-workflow.md          # 按这套模式写工程的落地步骤
│   ├── architecture.md              # Layered / DIP / Hybrid、Board 形态、构建强制
│   ├── concurrency-rules.md         # 任务/队列/优先级/同步原语/生命周期
│   ├── platform-profile.md          # 平台契约模板与查证方法
│   ├── rtos-api-semantics.md        # FreeRTOS / ESP-IDF / CMSIS 原生 API 语义差异
│   ├── esp-idf.md                   # ESP-IDF 启动、组件、IDF 6 迁移
│   ├── stm32-cubemx.md              # STM32/CubeMX、HAL、NVIC、EIDE/Keil
│   ├── callback-contexts.md         # ISR / 厂商事件 / 定时器 / 任务上下文
│   ├── timer-vs-task.md             # 周期任务 vs 软件定时器
│   ├── memory-and-stl.md            # 启动一次性分配、堆预算、STL / std::function 风险
│   ├── runtime-policies.md          # 失败降级、可观测性、时间、连接状态机
│   ├── module-matrix.md             # 逐模块层级、依赖与任务预算
│   └── product-architecture.md      # OTA/低功耗/看门狗/NVS/安全清单
├── templates/
│   ├── app/                         # Application + Display 模板（原生 RTOS API）
│   ├── devices/                     # Board / Device / DisplayPanel 纯接口
│   ├── esp-idf/                     # app_main、CMake、Kconfig、分区、sdkconfig
│   ├── stm32/                       # 启动模板、FreeRTOSConfig 说明
│   ├── style/                       # .clang-format / .editorconfig
│   └── host-test/                   # 主机侧测试与 mock 目录骨架
├── checklists/bring-up.md           # 唯一一份验收清单
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

```text
~/.copilot/skills/mcu-rtos-task-loop/
```

`references/`、`templates/`、`checklists/` 要和 `SKILL.md` 一起复制；只复制 `SKILL.md`
会丢失平台契约、命名标准与模板。

## 使用

在 VS Code 的 Copilot Chat 中描述要做的事，**同时给出目标平台**：

```text
ESP32-C3 + ESP-IDF 6.0，加一个 MQTT 上报模块，带异步连接与发送
```

```text
STM32F407 + FreeRTOS，把这个裸机工程改成 中断 → 队列 → 任务 的模式，RAM 只有 128KB
```

```text
帮我审查这个工程的 ISR、事件循环、分层和命名，按 skill 标准给偏差清单
```

推荐流程：

1. AI 识别 MCU/RTOS/SDK，按 `references/layers-and-naming.md` 判定每个模块的层；
2. 填写 `references/platform-profile.md`（20 项）与 `references/module-matrix.md`；
3. 先算 RAM 预算、配 RTOS 堆上限（`references/memory-and-stl.md`），
   再依次生成 `devices/services` 接口、`bsp` 实现、`app`；
4. 按 `checklists/bring-up.md` 做语义与构建检查。

## 覆盖内容

| 主题 | 位置 |
| --- | --- |
| 分层判据、模块归类、常见误判 | `references/layers-and-naming.md` |
| 按这套模式写工程的落地步骤 | `references/project-workflow.md` |
| 命名、风格、改造现有工程规则 | `references/layers-and-naming.md` |
| 格式落地（clang-format / editorconfig） | `references/layers-and-naming.md` §6 + `templates/style/` |
| Layered / DIP / Hybrid 与构建强制 | `references/architecture.md` |
| 中断 → 任务链路、回调上下文 | `SKILL.md` + `references/callback-contexts.md` |
| Application / MainEventLoop / 启动时序 | `SKILL.md` |
| APP 异步 API、队列与载荷策略 | `SKILL.md` + `references/concurrency-rules.md` |
| 任务、栈、同步原语、临界区 | `references/concurrency-rules.md` + `references/rtos-api-semantics.md` |
| 周期任务 vs 软件定时器 | `references/timer-vs-task.md` |
| 启动一次性分配、RTOS 堆预算、STL 策略 | `references/memory-and-stl.md` |
| 失败降级、可观测性、时间、连接状态机 | `references/runtime-policies.md` |
| 平台契约与查证方法 | `references/platform-profile.md` |
| ESP-IDF 6 启动、组件、API 迁移 | `references/esp-idf.md` |
| STM32/CubeMX、HAL、NVIC、EIDE/Keil | `references/stm32-cubemx.md` |
| 产品级架构（OTA/低功耗/看门狗/NVS/安全） | `references/product-architecture.md` |
| 主机侧测试与 mock | `templates/host-test/` |
| Application + Display 模板 | `templates/app/` |
| 验收清单 | `checklists/bring-up.md` |

## 平台适配说明

本技能不绑定具体 MCU。以下内容属于**平台语义**，必须在生成代码前确认，不能凭记忆：

- 调度器由谁启动、是否已启动；
- 任务栈与高水位的单位；
- RTOS 堆配置位置与上限；
- ISR API 与中断优先级规则；
- 临界区是否跨核；
- 定时器/厂商事件回调上下文；
- 构建系统、组件依赖、分区/内存限制。

代码直接调用所选用 RTOS 的原生 API；平台细节会随 SDK 版本变化，
`references/` 中的内容是参考，不是永久真理；使用前请对照目标 SDK 的头文件与迁移指南。

## 许可

本仓库未声明开源许可证。如需对外发布，请补充合适的 LICENSE。