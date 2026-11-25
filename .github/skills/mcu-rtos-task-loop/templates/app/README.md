# Application + Display 模板（直接用 RTOS 原生 API）

一套 APP 层模板，复制到 `components/app/`（ESP-IDF）或 EIDE/Keil 的 `app/` 分组。

```text
templates/app/
├── application.hpp/.cpp       # 组合根：主队列 + 主任务 + 主定时器
└── display.hpp/.cpp           # APP 模块：异步 API + 内部队列 + 绘制任务
```

依赖的接口在 `templates/devices/`：`board.hpp`、`device.hpp`、`display_panel.hpp`。

**不提供 `port/` 封装，也不提供静态分配版本。** 代码直接调用 FreeRTOS：

- ESP-IDF：`#include "freertos/FreeRTOS.h"`、`"freertos/queue.h"`、`"freertos/task.h"`、
  `"freertos/timers.h"`；
- 原生 FreeRTOS（STM32 等）：换成 `#include "FreeRTOS.h"`、`"queue.h"`、`"task.h"`、
  `"timers.h"`。

## 1. Display：外部只投递，内部绘制

```text
DrawText() / DrawValue() / Clear()
        │  PostXxx()（timeout = 0）
        ▼
Display 内部队列
        ▼
TaskLoop() → Handle(event)
        ├─ HandleDrawText()  → panel_->ShowText()
        ├─ HandleDrawValue() → snprintf + panel_->ShowText()
        └─ HandleClear()     → panel_->Clear()
```

公有 API：

```cpp
bool Init();                                     // 建内部队列
bool Start();                                    // 建绘制任务
bool DrawText(int x, int y, const char *text);   // 异步：返回是否入队
bool DrawValue(int x, int y, int32_t value);     // 异步
bool Clear();                                    // 异步
uint32_t DroppedCount() const;
```

- 公有 API 只做参数校验、拷贝、入队，**不直接访问 panel**。
- 数字格式化在 `HandleDrawValue()` 里做。
- `Event` / `Message` 全部 private，外部只能看到 `DrawXxx()`。
- 队列满 `dropped_` 计数，不阻塞调用者。

## 2. Application 如何用 Display

`Application` 持有 `Display display_`，由 `Board::GetDisplayPanel()` 注入：

```cpp
Application::Application()
    : board_(Board::GetInstance()), display_(board_.GetDisplayPanel()) {}

bool Application::Init() {
    if (!display_.Init()) { return false; }   // Display 内部队列
    queue_ = xQueueCreate(kQueueLength, sizeof(Event));
    timer_ = xTimerCreate("App Tmr", pdMS_TO_TICKS(kTimerPeriodMs), pdTRUE, this,
        &Application::TimerEntry);
    // ...
}

bool Application::Start() {
    if (!display_.Start()) { return false; }  // Display 绘制任务
    // xTaskCreate 主任务 / xTimerStart / 投递 kStart
}
```

主循环里只调异步 API：

```cpp
void Application::HandleStart() {
    (void)display_.DrawText(0, 0, "ready");
}

void Application::HandleTimerElapsed() {
    ++uptime_seconds_;
    (void)display_.DrawValue(0, 16, static_cast<int32_t>(uptime_seconds_));
}
```

## 3. 分配策略：启动时一次性分配

模板不做逐对象静态创建（`xTaskCreateStatic` / `xQueueCreateStatic` / `xTimerCreateStatic`）。
队列、任务、定时器全部来自 RTOS 堆，在 `Init()` / `Start()` 里一次创建完，
运行期不删除、不释放。RAM 受限平台的落地顺序：

1. 先把**不用 RTOS 堆**的 RAM 布置完：全局/静态对象、显示 framebuffer、DMA 缓冲、
   协议栈静态缓冲、大常量放 flash；
2. 剩余 RAM 配成 RTOS 最大堆（FreeRTOS `configTOTAL_HEAP_SIZE`，heap_4/heap_5；
   ESP-IDF 的堆是 IDF 自动管理的内部 RAM，不需要这个宏）；
3. 启动时一次性创建所有任务/队列/定时器/信号量；
4. 用 `xPortGetMinimumEverFreeHeapSize()` / `heap_caps_get_minimum_free_size()`
   验证水位，留 10% 或 1~2 KB 余量。

细节见 `references/memory-and-stl.md`。

## 4. 约束

- **栈单位按平台契约**：ESP-IDF 的 `xTaskCreate` 用 byte；原生 FreeRTOS 用 word。
  模板按 ESP-IDF 写（`kTaskStackBytes`）；STM32 改成 `kTaskStackWords` 并直接填 word。
- `Init()`/`Start()` 返回值必须检查；启动期分配失败是配置错误，记录后停机，
  不要带着空句柄继续跑。
- `Init()` 只建资源；`Start()` 只建任务/启动 timer，**不启动调度器**。
- Device 回调必须在任务上下文调用；ISR 先投递到 BSP 任务，再由 BSP 任务
  调用 `NotifyReceived()`。
- 定时器回调运行在 timer 任务：只投递，不阻塞、不 stop/delete 自身。

## 5. 构建

```cmake
idf_component_register(
    SRCS "src/application.cpp" "src/display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES devices services freertos
)
```

EIDE / Keil：把 `application.cpp`、`display.cpp` 与 FreeRTOS 内核文件加入工程，
`app/`、`bsp/` 按 C++17 编译，`-fno-exceptions -fno-rtti`。