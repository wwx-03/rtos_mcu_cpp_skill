# 周期任务 vs 软件定时器

## 1. 结论

默认选择**周期任务 + `port::DelayUntil`**（或 `vTaskDelayUntil`）：

- 逻辑独立，出问题不影响其他模块；
- 可以自由阻塞、打印、写 Flash、走总线；
- 可以单独设置优先级和栈大小。

只有同时满足下面条件时，才改用**共享软件定时器**：

- 采样/触发频率低（≤ 50 Hz）；
- 实例可能不止一个（按键、指示灯、低速输入轮询）；
- 单次工作量极小（读 IO/寄存器、比较状态、投递事件）；
- 回调严格只投递，不做业务。

典型场景：按键消抖、LED 闪烁节拍、低速输入轮询。

## 2. 决策表

| 条件 | 周期任务 | 共享软件定时器 |
| --- | --- | --- |
| 频率 > 100 Hz，或需要微秒级时序 | ✅ | ❌ |
| 回调内需要阻塞、延时、总线事务、写 Flash | ✅ | ❌ |
| 需要独立栈或独立优先级 | ✅ | ❌ |
| 低频、实例多、每个实例有独立状态 | 可用但浪费 | ✅ |
| 回调只读 IO + 投递事件 | 可用 | ✅ |
| 希望所有实例共享一个时间基准 | 不一定 | ✅（同一 `Tmr Svc`） |

## 3. 成本对比（FreeRTOS）

| 资源 | 周期任务 | 软件定时器 |
| --- | --- | --- |
| 每实例开销 | TCB + 独立栈（案例中按键任务为 2048 B 栈） | timer 控制块 + 回调上下文（几十字节） |
| 执行上下文 | 自己的任务 | 共享定时器任务（`Tmr Svc`） |
| 优先级 | 可单独设置 | 跟随定时器任务 |
| 能否阻塞 | 可以 | 不可以 |
| 故障隔离 | 好 | 差（一个回调卡住会影响所有 timer） |

> 具体字节数随 RTOS 与配置变化，必须以平台契约和目标 SDK 的配置为准；
> 上表是量级对比，不是固定值。

## 4. 按键采样案例

### 场景

GPIO 按键需要 20 ms 消抖并识别点击；回调最终只是向 `Application` 投递一个事件。

### 实现

- `Init()` 只配置 GPIO；
- `Start()` 记录初始电平，再创建并启动 20 ms 自动重载定时器；
- 回调只做三件事：读电平、和上次状态比较、投递；
- 用“上次按下 + 本次松开”识别一次点击；
- 停止/删除只放在初始化失败或关机路径。

```cpp
bool Button::Start() {
	pressed_ = Read();  // 记录初始电平，避免上电时误判
	port::TimerOptions options{"button", 20, true, this};
	if (!port::CreateTimer(&Button::OnTick, options, &timer_)) {
		return false;
	}
	return port::StartTimer(timer_, 0);
}

// 定时器任务上下文：只比较状态并投递，绝不阻塞
void Button::OnTick(void* ctx) {
	auto* self = static_cast<Button*>(ctx);
	const bool pressed = self->Read();
	if (self->pressed_ && !pressed) {
		self->PostClickEvent();  // 入队 timeout = 0
	}
	self->pressed_ = pressed;
}
```

### 扩展

- 按下/松开：比较 `last -> now` 的两个方向；
- 长按：记录按下时刻，在回调里比较单调时间；
- 双击：记录上次释放时刻与计数；
- 以上都保持“回调只投递”，业务处理留在 APP 任务里。

## 5. 使用软件定时器的硬约束

1. 回调运行在定时器任务：只投递，不阻塞、不打印、不写 Flash、不 stop/delete 自身。
2. 创建/启动放在 `Start()` 或 init 路径；运行期不反复 create/delete。
3. 明确 tick 频率与精度（例如 1 kHz tick 下，20 ms 是 20 个 tick）。
4. 确认定时器任务的优先级、栈、命令队列长度（`configTIMER_TASK_PRIORITY`、
   `configTIMER_TASK_STACK_DEPTH`、`configTIMER_QUEUE_LENGTH`）。
5. 回调里的投递用 `timeout = 0`；队列满计数丢弃，不能反压定时器任务。

## 6. 何时必须回到任务

- 采样频率高，或要求微秒级时间戳；
- 需要复杂手势状态机、滤波、浮点运算；
- 回调需要访问总线、文件系统、网络；
- 多个定时器可能同时触发且各自耗时；
- 需要独立优先级，避免被其他 timer 拖慢。

## 7. 平台契约要确认的项

- 定时器回调上下文：任务 / ISR / 可配置；
- 定时器任务的优先级、栈、命令队列长度；
- tick 频率与最小周期；
- `start/stop/delete` 是否可能阻塞，超时语义；
- 是否支持静态分配（`xTimerCreateStatic`）。

## 8. 与其他参考的关系

- 回调上下文矩阵见 `references/callback-contexts.md`；
- 栈单位与优先级语义见 `references/platform-profile.md`；
- 定时器/事件回调的失败与降级见 `SKILL.md` 第 11 节。
