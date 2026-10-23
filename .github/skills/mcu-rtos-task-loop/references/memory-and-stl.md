# 内存、分配与 STL 策略

## 1. 分配策略

| 场景 | 策略 | 注意 |
| --- | --- | --- |
| RAM 小、无 PSRAM、长期运行 | 静态创建 | 占用在链接期可见 |
| RAM 较大（ESP32 等） | `Init()` 阶段动态创建 | 检查返回值，失败即停机 |
| PSRAM / DMA 缓冲 | 平台 memory capability | 按平台 API 指定，不要假设普通堆可用 |
| 频繁创建/销毁 | 预分配池 + 位图 | 与平台无关，运行期零分配 |

无论哪种策略：

1. 不在 ISR 里分配；
2. 运行期不反复 create/delete；
3. 分配失败必须可见（assert/abort 或明确降级），禁止静默使用空句柄。

> `templates/port/rtos_port.hpp` 覆盖动态创建和静态 task/queue/timer
> （`port::StaticTask` / `StaticQueue` / `StaticTimer`）。
> 静态信号量仍需按 SDK 语义包装 `xSemaphoreCreateStatic`，
> 并在平台契约中记录缓冲区类型、对齐要求和栈单位（word 还是 byte）。
> 完整示例见 `templates/app/`。

## 2. C++ 侧规则

- 关闭异常/RTTI 时，`new` 失败可能直接 abort；需要可恢复时用 `new (std::nothrow)` 并检查。
- 函数内 `static` 的首次构造有 `__cxa_guard`；不要在 ISR 里首次构造。
- `volatile` 不是原子操作；不要写 `++volatile_var`（C++20 已废弃）。
  计数用 `std::atomic`、临界区或任务内单写者。
- 大字体表/常量表用 `constexpr` 放 flash，不要放 RAM。
- 不用 C 风格分配（`malloc/free`）与 C++ `new/delete` 混用。

## 3. 堆 / STL / `std::function`

| 平台 | 允许 | 限制 |
| --- | --- | --- |
| ESP32 / RAM 充足 | `std::string`、`std::map`、`std::function`（任务上下文） | 不在 ISR/高频路径使用；检查分配失败；热路径用固定缓冲/池 |
| STM32 / RAM 紧张 | 固定数组、`etl` 容器、函数指针 + `void*` context | 避免捕获式 `std::function`、频繁 `std::string`、运行期容器增删 |
| 所有平台 | 静态表、`constexpr` 常量 | 不在 ISR 分配；不在回调里做大块 JSON/字符串拼接 |

`std::function` 可能因捕获而分配；`std::string`/`std::map` 也可能分配。
如果平台契约要求“无运行期 malloc”，这些接口必须替换为函数指针 + context
或固定容量容器，并在模块矩阵中记录。

## 4. 常见错误

| 错误 | 正确 |
| --- | --- |
| 在 ISR / 定时器回调里 `new`、拼接字符串 | 只投递，任务里处理 |
| 运行期反复 create/delete | `Init()` 一次创建，运行期复用 |
| 空句柄继续使用 | 检查返回值 + `ready_` 标志 |
| `++volatile` 当原子计数 | `std::atomic` / 临界区 / 单写者 |
| 大表放 RAM | `constexpr` / `static const` 放 flash |
