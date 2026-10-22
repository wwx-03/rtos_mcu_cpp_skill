# 模块矩阵（Hybrid / DIP 必填）

Hybrid 和 DIP 的核心不是“画一张分层图”，而是**逐模块写清楚依赖路径和并发归属**。
生成代码前先填这张表；否则“复杂模块走接口、简单模块用具体类”只是一句无法验收的口号。

## 1. 模块表

| 模块 | 变体/易变度 | 对外接口 | 实现位置 | 归属任务 | 队列/载荷 | 回调上下文 | 分配策略 | 生命周期 | app 可 include |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 例：WiFi | 高（配网/重连） | `services/network.hpp` | `bsp/wifi_service.*` | `wifi_app` | 命令队列，≤8 项 | 系统事件任务只投递 | init 阶段动态 | `Start/Stop/Reconfigure` | 否（只 include services） |
| 例：MQTT 传输 | 高（协议栈） | `services/message_transport.hpp` | `bsp/esp_mqtt_transport.*` | `mqtt_transport` | 消息队列/事件 | MQTT 任务只投递 | init 阶段动态 | `Start/Stop/Publish/Subscribe` | 否 |
| 例：云端 JSON 协议 | 高（业务） | 无（app 内部类） | `app/cloud_protocol.*` | `cloud_protocol` | 消息队列 | 传输层回调只投递 | 字符串/JSON（按平台策略） | `Init/Start/Stop` | 不适用（app 内部） |
| 例：LED | 低 | `devices/led.hpp` | `bsp/strip_led.*` | `led_app` | 任务通知 | timer/task | 静态配置 | `Start` + 状态通知 | 否（通过 devices） |
| 例：GPIO 锁 | 极低 | 无（具体类） | `bsp/gpio_lock.*` | 调用者任务 | 无 | 无 | 静态 | 构造即初始化 | 是（Hybrid 简单模块例外） |

规则：

1. 同一模块只能有一条路径：要么走接口，要么走具体类，不能 app 和 bsp 同时直接使用。
2. 走接口的模块，app 不得 include `bsp/`；实现层头文件应放在 `src/` 并用 `PRIV_INCLUDE_DIRS` 私有化。
3. 简单模块允许 app 直接使用具体类时，必须写进“app 可 include”列，并接受换板/单测时的修改成本。
4. 回调上下文必须写清楚是 ISR、厂商事件任务、timer task 还是 APP 任务。
5. 分配策略必须写清楚：静态、init 阶段动态、池化；运行期不允许反复创建/销毁。

## 2. 任务预算表

| 任务 | 栈（字节） | 优先级 | 周期/触发 | 阻塞点 | 归属模块 |
| --- | --- | --- | --- | --- | --- |
| `main_loop` | 4096–6144 | 4 | 事件驱动 | 主队列 | Application |
| `network_app` | 4096–6144 | 3 | 事件驱动 | 命令队列 | Network |
| `sensor_app` | 3072–4096 | 2 | 1000 ms | 队列/DelayUntil | Sensor |
| `led_app` | 2048–3072 | 1 | 50 ms 或通知 | TaskNotify | LED |

规则：

- **一个并发域/资源 owner 一个任务**，不是“一个类一个任务”。
- 低频、实例多、回调只投递的模块，优先共享软件定时器或主事件循环，避免为每个实例建栈。
- 任务数、栈、优先级必须在平台契约和本表中同时记录；用高水位实测后再调整。

## 3. 构建约束检查

- [ ] 走接口的模块，app 组件不 `REQUIRES bsp`，也不 include bsp 头文件。
- [ ] 实现层没有公共头文件，或公共头只包含接口类型。
- [ ] 在 app 中临时 include 一个不允许的层，构建必须失败。
- [ ] 只编译一个具体板级 `.cpp`；多个会重复符号，缺失会 undefined reference。
- [ ] 每个模块的队列满/丢弃/重连都有计数和日志。
