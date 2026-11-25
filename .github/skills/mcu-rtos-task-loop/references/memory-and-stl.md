# 内存、分配与 STL 策略

## 1. 唯一策略：启动时一次性分配

**先把不用 RTOS 堆的 RAM 布置完，再把剩余 RAM 配成 RTOS 最大堆，然后在启动时
一次性创建所有 RTOS 对象，运行期不再申请、不再释放。**

| 阶段 | 做什么 | 不做什么 |
| --- | --- | --- |
| 链接期 | 全局/静态对象、常量表、framebuffer、DMA 缓冲、主栈 | 不放任务栈和队列控制块 |
| `main()` / `app_main()` 早期 | 配置 RTOS 堆上限（`configTOTAL_HEAP_SIZE` 或等价配置） | 不做业务分配 |
| `Init()` | 创建队列/信号量/定时器 | 不创建任务 |
| `Start()` | 创建全部任务、启动定时器 | 不启动调度器（ESP-IDF 已启动） |
| 运行期 | 只用已有对象；数据缓冲用固定数组/预分配池 | 不 create/delete/free，不做运行期扩容 |

为什么不做逐对象静态创建：

- `StaticTask_t` / `StaticQueue_t` / `StaticTimer_t` 的尺寸和对齐随 RTOS 版本变化，
  为每个对象维护 control block 数组既啰嗦又易错；
- “堆在启动时分配完、之后不再申请”在启动后同样确定：地址固定、水位可测；
- 只需要一个可调参数（堆上限），RAM 预算更好核对。

> 约束对象是**本工程创建的 RTOS 对象与业务缓冲**。ESP-IDF 的 WiFi/TCP-IP 等协议栈
> 在初始化或连接时内部会分配缓存，不受这条约束；把这些初始化放进启动流程，
> 让内存不足尽早暴露，并预留余量。

## 2. RAM 受限平台：先算账，再配堆

### 步骤 1：列出所有不用 RTOS 堆的 RAM

| 项目 | 来源 | 说明 |
| --- | --- | --- |
| `.data` / `.bss` | 全局对象、`Application` 单例、驱动对象 | 用 `size` / map 文件实测 |
| 大缓冲 | 显示 framebuffer、DMA、协议栈静态缓冲 | 能放 PSRAM/外部 RAM 就不占内部 RAM |
| 常量表 | 字库、图片、校准表 | `constexpr` / `const`，留在 flash |
| 主栈 | MSP / 启动栈 | 启动文件和链接脚本决定 |
| 保留 | 未来增长、对齐、RTOS 内部开销 | 留 10% 或 1~2 KB |

### 步骤 2：把剩余 RAM 配成 RTOS 堆

原生 FreeRTOS（STM32 / Cortex-M）：

```c
// FreeRTOSConfig.h；heap_4.c / heap_5.c 使用
#define configSUPPORT_DYNAMIC_ALLOCATION  1
#define configSUPPORT_STATIC_ALLOCATION   0
#define configTOTAL_HEAP_SIZE   ((size_t)(100 * 1024))   // 剩余 RAM，留余量
#define configUSE_MALLOC_FAILED_HOOK       1
```

- `configTOTAL_HEAP_SIZE` 是堆的上限，`ucHeap` 落在 `.bss`；填太大直接链接失败，
  这是好事——比运行期才发现更容易定位。
- RAM 不连续（多块 SRAM/CCM）时用 `heap_5.c`，启动时先 `vPortDefineHeapRegions()`
  定义所有区域，**再**创建任何 RTOS 对象。
- CCM/DTCM 等不能给 DMA 的区域可以用 heap_5 单独划出（先查芯片手册）。

ESP-IDF（ESP32 系列）：

- 没有 `configTOTAL_HEAP_SIZE`；IDF 把未被静态占用、未被预留的内部 RAM 组成系统堆，
  `xTaskCreate` / `xQueueCreate` 都从这里分配。
- 大块数据用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`（有 PSRAM 时）或
  `MALLOC_CAP_INTERNAL` 明确能力，不要把大缓冲默认丢进内部 RAM。
- 启动时打印 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`，
  与启动完成后对比，确认没有运行期增长。

### 步骤 3：启动时一次创建全部对象

```cpp
bool Application::Init() {
	queue_ = xQueueCreate(kQueueLength, sizeof(Event));
	timer_ = xTimerCreate("App Tmr", pdMS_TO_TICKS(kTimerPeriodMs), pdTRUE, this,
		&Application::TimerEntry);
	return queue_ != nullptr && timer_ != nullptr;
}

bool Application::Start() {
	if (xTaskCreate(&Application::TaskEntry, "app_main", kTaskStackBytes, this,
			kPriority, &task_) != pdPASS) {
		task_ = nullptr;
		return false;
	}
	return xTimerStart(timer_, 0) == pdPASS;
}
```

- 启动失败 = 预算算错：记录哪个对象失败，然后停机/复位；不要带着空句柄继续跑。
- 队列长度、任务数、栈大小都写进模块矩阵；启动后再改就要改预算。

### 步骤 4：验收水位

```c
// 启动完成后运行一段时间，确认堆不再下降
size_t free_now = xPortGetFreeHeapSize();
size_t free_min = xPortGetMinimumEverFreeHeapSize();
```

- `free_min` 是“历史最低”，运行一段时间后应基本稳定；
- 仍然持续下降说明有运行期分配（日志、JSON、`std::string`、隐式 `new`），要揪出来；
- 保留量建议 ≥ 10% 或 1~2 KB，取大者。

预算示例（128 KB SRAM，仅示意）：

| 项目 | 占用 |
| --- | --- |
| `.data` / `.bss` + 驱动对象 | 12 KB |
| 显示 framebuffer + DMA 缓冲 | 5 KB |
| 主栈 MSP | 2 KB |
| 保留 | 9 KB |
| **RTOS 堆（任务栈 + 队列 + 信号量 + 定时器）** | **100 KB** |

## 3. C++ 侧规则

- 关闭异常/RTTI 时，`new` 失败可能直接 abort；需要可恢复时用 `new (std::nothrow)` 并检查。
- 函数内 `static` 的首次构造有 `__cxa_guard`；不要在 ISR 里首次构造，
  也不要在启动路径之外首次构造业务单例。
- `volatile` 不是原子操作；不要写 `++volatile_var`（C++20 已废弃）。
  计数用 `std::atomic`、临界区或任务内单写者。
- 大字体表/常量表用 `constexpr` 放 flash，不要放 RAM。
- 不用 C 风格分配（`malloc/free`）与 C++ `new/delete` 混用。
- 动态容器只在启动阶段构建；运行期只做固定容量操作。

## 4. 堆 / STL / `std::function`

| 平台 | 允许 | 限制 |
| --- | --- | --- |
| ESP32 / RAM 充足 | `std::string`、`std::map`、`std::function`（任务上下文） | 不在 ISR/高频路径使用；不在稳定运行期扩容；热路径用固定缓冲/池 |
| STM32 / RAM 紧张 | 固定数组、`etl` 容器、函数指针 + `void*` context | 避免捕获式 `std::function`、频繁 `std::string`、运行期容器增删 |
| 所有平台 | 静态表、`constexpr` 常量 | 不在 ISR 分配；不在回调里做大块 JSON/字符串拼接 |

`std::function` 可能因捕获而分配；`std::string`/`std::map` 也可能分配。
如果平台契约要求“启动后无分配”，这些接口必须替换为函数指针 + context
或固定容量容器，并在模块矩阵中记录。

## 5. 常见错误

| 错误 | 正确 |
| --- | --- |
| 在 ISR / 定时器回调里 `new`、拼接字符串 | 只投递，任务里处理 |
| 为每个对象写静态 control block 数组 | 启动时从 RTOS 堆一次创建 |
| 运行期反复 create/delete / `free` | `Init()`/`Start()` 一次创建，运行期复用 |
| 空句柄继续使用 | 检查返回值 + `ready_` 标志 |
| 先创建 RTOS 对象，再改 `configTOTAL_HEAP_SIZE` | 先算预算，再配堆，最后启动创建 |
| 启动后堆水位持续下降 | 找出运行期分配（日志/JSON/`std::string`/隐式 `new`） |
| `++volatile` 当原子计数 | `std::atomic` / 临界区 / 单写者 |
| 大表放 RAM | `constexpr` / `static const` 放 flash |