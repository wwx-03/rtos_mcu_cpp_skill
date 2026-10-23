# Application + Display 模板：静态分配 vs 动态分配

两套完整的 APP 层模板，**二选一复制到工程**：

```text
templates/app/
├── static_task/                 # StaticQueue + StaticTask + StaticTimer
│   ├── application.hpp/.cpp
│   └── display.hpp/.cpp
└── dynamic_task/                # port::CreateXxx（无模板）
    ├── application.hpp/.cpp
    └── display.hpp/.cpp
```

依赖的接口在 `templates/devices/`：`board.hpp`、`device.hpp`、`display_panel.hpp`。

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
    // 建主队列 / 主 timer
}

bool Application::Start() {
    if (!display_.Start()) { return false; }  // Display 绘制任务
    // 建主任务 / 启动 timer / 投递 kStart
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

## 3. 静态 vs 动态

| 项 | `static_task` | `dynamic_task` |
| --- | --- | --- |
| 主队列 / 主任务 / 主 timer | `StaticQueue` / `StaticTask` / `StaticTimer` | `QueueHandle` / `TaskHandle` / `TimerHandle` |
| Display 队列 / 任务 | `StaticQueue` / `StaticTask` | `QueueHandle` / `TaskHandle` |
| 创建 | `queue_.Create()` / `task_.Start()` / `timer_.Create()` | `port::CreateQueue` / `CreateTask` / `CreateTimer` |
| ISR 发送 | `queue_.SendFromIsr()` | `port::QueueSendFromIsr()` |
| 失败时机 | 编译期 `static_assert` + 返回值 | 运行期返回值 |

`MainEventLoop()` / `HandleEvent()` / `HandleXxx()` 结构相同，只是队列调用不同。

## 4. 约束

- 静态版要求 `configSUPPORT_STATIC_ALLOCATION = 1`；`Application` 含主任务 +
  Display 任务两份栈（约 7 KB），必须全局/静态，不能放任务栈上。
- 动态版要求 `configSUPPORT_DYNAMIC_ALLOCATION = 1`；`Init()`/`Start()` 返回值必须检查。
- `Init()` 只建资源；`Start()` 只建任务/启动 timer，**不启动调度器**。
- Device 回调必须在任务上下文调用；ISR 先投递到 BSP 任务，再由 BSP 任务
  调用 `NotifyReceived()`。

## 5. 构建

```cmake
idf_component_register(
    SRCS "src/application.cpp" "src/display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES port devices
)
```

动态版把 `static_task` 换成 `dynamic_task`；**不要同时加入两个目录**。
EIDE / Keil：文件列表里只挂其中一套 `.cpp`。