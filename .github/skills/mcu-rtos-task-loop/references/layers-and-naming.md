# 分层归属与命名标准

> 本文件是**唯一标准**。生成或改造代码时，层级与命名以本文件为准。
> 现有工程不一致时：新增/改动的符号按本文件，现有偏差写进“偏差清单”，不批量重命名。

## 1. 六层职责

| 层 | 职责 | 可以依赖 | 禁止 |
| --- | --- | --- | --- |
| `app/` | 业务状态机、协议报文、事件派发 | `devices`/`services` 接口、`port`；Hybrid 下可用简单 `bsp` 具体类 | 寄存器、HAL/IDF 调用、平台 `#ifdef`、具体板头 |
| `services/` | 平台能力接口（网络/存储/时间/OTA/加密） | 仅标准库 | RTOS、厂商头、具体引脚、业务报文 |
| `devices/` | 器件抽象、纯协议/算法接口 | 仅标准库 | RTOS、厂商头、网络、存储、时间、任务/队列 |
| `bsp/` | 具体驱动、板级组合根、ISR/厂商回调 | `devices`/`services`/`port`、厂商 SDK | 业务状态机、上层类型、反向依赖 `app` |
| `utility/` | 纯函数、数据结构（滤波/CRC/格式化/环形缓冲） | 仅标准库 | 全局状态、IO、时间、RTOS |
| `port/` | RTOS/SDK 薄封装（任务/队列/信号量/临界区/时间） | RTOS/SDK | 业务逻辑、器件语义 |

依赖方向（Hybrid 默认）：

```text
app → devices / services ← bsp → port → RTOS / SDK
app ──────────────────────→ port
utility ← 所有层（只被依赖，不反向依赖）
```

`Board` 是组合根，只在 `main()`/`app_main()` 使用。

## 2. 判定顺序（按顺序问，第一个命中即定层）

1. 是任务/队列/临界区/时间/调度器这类**平台语义封装**？→ `port/`
2. 绑定了**具体引脚、总线实例、中断号、厂商回调**？→ `bsp/`（实现 `devices`/`services` 接口）
3. 有**芯片型号、数据手册、寄存器或固定命令集**？→ `devices/`
4. 是 SDK 提供的**平台能力**，涉及连接/状态/凭据/重试/异步事件，且跨器件？→ `services/`
5. 是**业务规则、流程、协议报文、状态机**，换硬件不变？→ `app/`
6. 是**纯函数/数据结构**，无 IO、无时间、无 RTOS？→ `utility/`
7. 都不满足 → 先拆分；不允许“放哪都行”。

**device 与 service 的分界线**：`devices` 是“一个器件”，`services` 是“一种平台能力”。

- 有芯片、有总线、有数据手册 → `devices`
- 由 SDK 提供、有连接/凭据/重试/异步事件 → `services`

## 3. 模块归类表

| 模块 | 层 | 接口 | 实现 |
| --- | --- | --- | --- |
| WiFi 连接 / 配网 / 重连 | `services` | `services/network.hpp` | `bsp/wifi_station.cpp` |
| MQTT / HTTP / WebSocket | `services` | `services/message_transport.hpp` | `bsp/esp_mqtt_transport.cpp` |
| NVS / 文件 / 配置存储 | `services` | `services/settings_store.hpp` | `bsp/nvs_settings_store.cpp` |
| SNTP / RTC 校时 | `services` | `services/clock.hpp` | `bsp/sntp_clock.cpp` |
| OTA | `services` | `services/ota.hpp` | `bsp/esp_ota.cpp` |
| 加密 / 密钥 | `services` | `services/crypto.hpp` | `bsp/mbedtls_crypto.cpp` |
| 显示面板 | `devices` | `devices/display.hpp` | `bsp/ssd1306_panel.cpp` |
| 传感器（I2C/SPI/ADC 器件） | `devices` | `devices/ph_sensor.hpp` | `bsp/ads1115_ph.cpp` |
| 按键 / 编码器 / 继电器 / LED | `devices` | `devices/button.hpp` | `bsp/gpio_button.cpp` |
| 电机驱动 | `devices` | `devices/motor.hpp` | `bsp/drv8833_motor.cpp` |
| 看门狗（芯片外设） | `devices` | `devices/watchdog.hpp` | `bsp/iwdg_watchdog.cpp` |
| JSON / CRC / 滤波 / 定点格式化 / 环形缓冲 | `utility` | `utility/*.hpp` | 同名 `.cpp` |
| 命令分发 / 业务状态机 / 上报策略 | `app` | `app/*.hpp` | `app/*.cpp` |

**接口长什么样（device vs service）**：

```cpp
// services/network.hpp —— 平台能力：连接/凭据/重试，不含引脚
class Network {
public:
	virtual bool Connect() = 0;
	virtual bool IsConnected() const = 0;
	virtual bool Publish(const char *topic, const char *payload) = 0;
};

// devices/display_panel.hpp —— 一个器件：坐标、像素、刷新
class DisplayPanel {
public:
	virtual bool ShowText(int x, int y, const char *text) = 0;
};
```

判断口诀：**有芯片/总线/数据手册 → device；有连接/凭据/重试 → service。**

**接口在 `devices`/`services`，实现永远在 `bsp`。** app 不 include `bsp/`
实现头；Hybrid 的简单器件例外见 `references/architecture.md`。

## 4. 常见误判

| 误判 | 为什么错 | 正确 |
| --- | --- | --- |
| WiFi/MQTT 放进 `devices/` | 不是单个器件，是平台能力 | `services/` |
| 工程里没有 `services/` 目录 | 网络/存储/时间/OTA 无处安放 | 建 `services/`，不要塞进 `devices/` 或 `app/` |
| 把 NVS 封装成 device | 存储是平台能力 | `services/` |
| 业务 JSON 组包放 `bsp/` | 换硬件不变的是业务 | `app/` |
| 滤波/换算放 `bsp/` | 纯算法应可主机测试 | `utility/` 或 `devices/` 内的纯函数 |
| 同一模块既有 `devices` 公共头又有 `bsp` 公共头 | 出现两条依赖路径 | 接口在 `devices`，实现头放 `src/` 私有 |
| `services` 里写寄存器 | services 应平台无关 | 下沉到 `bsp/` 实现 |
| `devices` include FreeRTOS | 器件抽象不该知道 RTOS | 只在 `bsp/`/`port/` |
| `Board` 暴露具体驱动类型 | app 依赖实现细节 | 暴露 `devices`/`services` 接口 |
| 按外设拆 `uart_task`/`gpio_task` | 任务应按资源 owner 拆 | 一个并发域一个任务 |

## 5. 命名表

| 对象 | 规则 | 示例 |
| --- | --- | --- |
| 目录 | snake_case，固定六层 | `app/ bsp/ devices/ services/ utility/ port/` |
| 文件 | snake_case，一名一类 | `water_monitor.hpp` / `water_monitor.cpp` |
| 接口头 | `include/<layer>/xxx.hpp` | `devices/include/devices/display.hpp` |
| 实现源 | `src/xxx.cpp` | `bsp/src/ssd1306_panel.cpp` |
| 实现私有头 | `src/xxx_internal.hpp`，不对外暴露 | `bsp/src/wifi_station_internal.hpp` |
| 接口类 | 名词，**不加 `I` 前缀** | `DisplayPanel`、`Network`、`SettingsStore` |
| 实现类 | `<芯片/能力><功能>` | `Ssd1306Panel`、`Esp32WifiNetwork`、`NvsSettingsStore` |
| 具体板 | `<Mcu><BoardName><Version>Board` | `Esp32SmartLockV01Board` |
| APP 类 | 领域名词，不加 `App`/`Manager`/`Helper` 后缀 | `WaterMonitor`、`Display` |
| 方法 | PascalCase，动词开头 | `DrawText()`、`HandleRxCpltEvent()` |
| 自由函数 | PascalCase；`create_new_board()` 为固定例外 | `ToTicks()`、`TimerTrampoline()` |
| 数据成员（类） | snake_case + 后缀 `_` | `queue_`、`ready_` |
| POD 载荷字段 | snake_case，无后缀 | `event`、`sample`、`text` |
| 局部变量 / 参数 | snake_case | `stack_words`、`text` |
| 句柄 | 类型后缀 | `main_queue_`、`led_task_`、`bus_mutex_`、`key_timer_` |
| 布尔成员 | 形容词/过去分词 | `ready_`、`connected_` |
| 常量 | `constexpr kPascalCase` | `kMaxSamples`、`kDefaultTimeoutMs` |
| 宏 | `UPPER_SNAKE`，仅限条件编译 | `CONFIG_*`、`ESP_PLATFORM` |
| 事件 | `enum class XxxEvent : uint32_t`，值 `kXxx` | `kShowText`、`kWifiConnected` |
| 状态 | `enum class State : uint8_t`，值 `kXxx` | `kIdle`、`kConnecting` |
| 载荷结构 | `struct XxxSample` / `XxxCommand` | `WaterQualitySample` |
| 类型别名 | `using Xxx = ...;`，PascalCase | `using SampleBuffer = ...;` |
| 命名空间 | 仅 `port`；文件内用匿名 namespace | `namespace { ... }` |
| ISR | `<来源>Isr` | `SensorReadyIsr` |
| 任务入口 | `static void TaskEntry(void *ctx)` + `TaskLoop()` | |
| 定时器回调 | `static void OnTimer(void *ctx)` / `OnTick` | |
| 事件处理 | `HandleXxx()` | `HandleWifiDisconnected()` |
| 内部投递 | `PostXxx()` | `PostWaterQualityEvent()` |
| 只读查询 | `IsXxx()` / `HasXxx()` | `IsConnected()` |

### 生命周期只允许三个名字

| 方法 | 职责 |
| --- | --- |
| `bool Init()` | 创建队列/信号量/定时器，注册回调；**不创建任务** |
| `bool Start()` | 创建任务、启动定时器；**不启动调度器** |
| `void Stop()` | 请求停止、等待任务退出、释放资源（仅关机/重配路径） |

禁止 `Begin/Setup/Run/Open/Close/Deinit` 等同义词混用。

### 命名反例

`doThing`、`m_foo`、`p_foo`、`IThing`、`ThingManager`、`ThingHelper`、
`ThingUtil`、`xyzTask`（自由函数）、匈牙利前缀、随意缩写。

## 6. 风格细则

- 缩进 Tab，K&R 花括号，`#pragma once`。
- include 顺序：自身头文件 → C 标准库 → C++ 标准库 → 项目内 → 厂商 SDK。
- 组件内 include 用根路径：`#include "devices/display.hpp"`。
- 头文件不写 `using namespace`；只放声明、`inline`/`constexpr`/模板。
- 一个文件一个类；具体板类只在 `.cpp` 内定义。
- 用 `nullptr`、`static_cast`；不用 C 风格转换；单参构造加 `explicit`；
  重写方法写 `override`；具体板类写 `final`。
- 用 `enum class`、`constexpr`，不用 `#define` 定义常量。
- 不用异常/RTTI 的平台上不要依赖它们；需要可恢复分配时用 `new (std::nothrow)`。
- ISR/热路径不用捕获式 `std::function`、`std::string`、STL 增删。
- 注释中文，只写非显然的“为什么”；不保留注释掉的代码。
- 不写魔法数字，先命名常量。

## 7. 改造现有工程

1. **盘点**：列出现有目录、模块、命名、构建系统，不要先写代码。
2. **映射表**：`现有模块 → 层 → 接口 → 实现 → 处理（保留/移动/新建/合并）`。
3. **最小 diff**：只改与本任务相关的文件；不批量重命名、不调整无关格式。
4. **新代码 100% 按本文件命名**；现有偏差列“偏差清单”，本次不修。
5. **缺 `services/` 就补**：本任务涉及的 网络/存储/时间/OTA 模块从 `devices`/`app`
   迁到 `services` 接口 + `bsp` 实现；无关模块只进偏差清单，不在本次迁移。
6. **冲突规则**：新文件、新符号、正在改的符号以本文件为准；
   不允许用“跟现有工程保持一致”作为沿用错误命名的理由。

### 偏差清单模板

```markdown
| 现有符号 / 位置 | 违反项 | 本次处理 | 后续建议 |
| --- | --- | --- | --- |
| `bsp/wifiTask` | 自由函数、驼峰 | 保留不动，新增 `Wifi::TaskLoop()` | 统一重命名 |
| `devices/mqtt_client.hpp` | 平台能力放错层 | 新增 `services/message_transport.hpp`，旧文件标 deprecated | 迁到 bsp 实现 |
```

## 8. 审查输出格式

审查分层/命名时按下面格式给结论，避免只给感受：

| 文件:行 | 类型 | 问题 | 建议 | 严重度 |
| --- | --- | --- | --- | --- |
| `app/network.cpp:42` | 分层 | app 里直接调用 `esp_mqtt_client_*` | 抽 `services/message_transport.hpp` + `bsp` 实现 | P0 |
| `bsp/wifi.cpp:17` | 命名 | `wifiTask` 自由函数 | `Wifi::TaskLoop()` | P1 |

规则：先列 P0（会导致错误行为/无法测试），再列 P1（一致性问题）；
每条给最小修改建议；不要求全量重命名。
