# 任务、队列与同步原语

**直接调用所用 RTOS 的原生 API**（FreeRTOS：`xTaskCreate` / `xQueueCreate` / `xTimerCreate`；
CMSIS-RTOS2：`osThreadNew` / `osMessageQueueNew` / `osTimerNew`），不要再包一层
`port::CreateTask` 之类的转接。平台差异（栈单位、ISR API、临界区语义）写进平台契约。

## 1. 任务设计

- **一个并发域/资源 owner 一个任务**，不是“一个 APP 类一个任务”，也不是“一个外设一个任务”。
  同一队列/周期/生命周期的模块应合并；低频轮询优先共享软件定时器或主循环。
- 任务预算写进 `references/module-matrix.md`：任务数、栈、优先级、周期、阻塞点。
- 优先级表达实时性差异，不是重要性：

| 任务 | 优先级 | 说明 |
| --- | --- | --- |
| 硬实时控制 | 5 | 周期 + `vTaskDelayUntil` |
| main（`MainEventLoop`） | 4 | 只派发，快速返回 |
| network / 协议 | 3 | 阻塞在队列 |
| 采集 / storage | 2 | 周期或队列 |
| display / LED | 1 | 慢速、可被抢占 |
| Idle | 0 | 系统 |

- 高优先级任务必须会阻塞，否则低优先级任务饿死。
- 周期任务用 `vTaskDelayUntil`（`xTaskDelayUntil`），不要用 `vTaskDelay` 累积漂移；
  不要忙等，不要在任务里用厂商 `HAL_Delay`。
- 小 RAM 平台先做任务预算：减少任务数、共享 timer/主循环，再谈优先级。

### 栈大小

- 单位以平台契约为准：**ESP-IDF 的 `xTaskCreate` 用 byte；原生 FreeRTOS 用 word
  （`StackType_t`）**。写代码前查 `task.h`，不要凭印象。
- 起点：纯逻辑 2 KB，带日志/浮点 4~6 KB；用 `uxTaskGetStackHighWaterMark` 实测后
  留 1.5~2 倍余量（注意 ESP-IDF 返回 byte，原生 FreeRTOS 返回 word）。
- 日志、`snprintf`、浮点格式化都很吃栈；不要用“感觉”定栈。

## 2. 队列与载荷

- 任务间传数据用队列，值拷贝；队列元素保持小。
- 低频状态快照（20~50 B）可直接值拷贝；高频/大块数据用槽位池、stream buffer
  或地址/索引传递。
- 公有异步 API 入队 `timeout = 0`（`xQueueSend(queue, &item, 0)`）；队列满计数，
  不阻塞调用者。
- 队列深度按“最坏突发 + 消费速率”估算，不要随手 1 或 100。

## 3. 同步原语选择

| 需求 | 用什么 | 注意 |
| --- | --- | --- |
| 任务间传数据 | `xQueueCreate` / `xQueueSend` / `xQueueReceive` | 值拷贝；大块传索引 |
| APP 对外能力 | 异步 API + 自己的队列 | 默认做法 |
| 通知“有事件” | `xTaskNotifyGive` / `ulTaskNotifyTake`；或二值信号量 | 一对一优先任务通知 |
| ISR 通知任务 | `xQueueSendFromISR` / `vTaskNotifyGiveFromISR` | 末尾 `portYIELD_FROM_ISR(woken)` |
| 保护共享资源 | `xSemaphoreCreateMutex` | 只能在任务里获取 |
| 极短同核临界区 | `taskENTER_CRITICAL` / `taskEXIT_CRITICAL` | 不调用 RTOS API，不跨核 |
| 多任务等待同一条件 | `xEventGroupCreate` | 注意清除位与竞态 |

CMSIS-RTOS2 对应 `osMessageQueueNew/Put/Get`、`osSemaphoreNew/Release`、
`osThreadFlagsSet/Wait`；语义差异见 `references/rtos-api-semantics.md`。

## 4. 临界区与原子

- 临界区要短，里面不打印、不分配、不调用可能阻塞的 API。
- `taskENTER_CRITICAL` 只关当前核中断；双核共享数据用 spinlock/原子操作。
- ESP-IDF 6 的 `taskENTER_CRITICAL()` 需要 `portMUX_TYPE` 参数，单核也一样。
- `volatile` 不是原子操作；计数用 `std::atomic`、临界区或任务内单写者。
- 互斥量不能递归滥用；不要用互斥量保护 ISR 共享数据（ISR 不能拿互斥量）。

## 5. MainEventLoop 规则

- 主循环 handler 必须快速返回：只更新状态和**投递**，重活交给 APP 任务。
- 模块内部事件走各自队列；主队列只放中断事件和跨模块事件。
- 队列满时计数（`++dropped_`），不要静默丢弃。
- `Application::Start()` 只创建任务/启动 timer，不启动调度器；`Init()` 只建队列/timer/注册回调。

## 6. 生命周期

1. 资源只在 `Init()` 创建；任务只在 `Start()` 创建。
2. 所有 RTOS 对象在启动时一次性创建，运行期不 create/delete/free；
   需要重配时走 `Stop()` → `Init()`（仅关机/重配路径）。
3. 启动创建失败按预算错误处理：记录失败对象并停机/复位，不带着空句柄继续跑。
4. 停止顺序：先停生产者（ISR/定时器/协议栈回调），再停任务，最后释放资源。
5. 删除任务/队列后置空句柄并清 `ready_`，确保没有回调还引用它。