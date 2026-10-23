# 架构模式与依赖强制

本文件承接 `SKILL.md` §0：三种架构模式的目录、`Board` 形态、构建约束与反模式。
**默认 Hybrid**；只有用户明确要求时才用 Layered 或 DIP。

## 1. Layered（分层）

```text
app → bsp → devices → utility
 │      │
 └──────┴──→ port → RTOS / SDK
```

> `devices` 不应依赖 `port`/RTOS；`port` 由 `app` 和 `bsp` 使用。

- `app` 直接使用 `bsp` 暴露的设备/服务类；`devices` 是 `bsp` 的实现细节。
- 优点：文件少、间接层少、bring-up 快、Flash/RAM 占用通常更小。
- 缺点：`app` 与具体 BSP 耦合；换器件/换板/单测时修改面更大。
- 适用：单板、单 MCU、器件固定、小团队、ROM/RAM 紧、无单测/多板需求。

## 2. DIP / Ports & Adapters

```text
app → devices / services ← bsp
             ↑
      abstract Board（devices）
             ↑
  Esp32SmartLockV01Board（bsp，按板命名）
```

- `devices/`：纯接口 + 纯协议/算法；不 include RTOS、厂商头或上层。
- `services/`：WiFi / MQTT / 存储 / 时间等平台服务接口。
- `bsp/`：实现 `devices`/`services` 接口，只做 IO context、ISR、厂商回调、总线事务。
- `app`：只依赖 `devices`/`services` 接口，不得 include `bsp/` 下任何头。
- 优点：可 mock/主机单测、可换板/换器件、边界清晰。
- 缺点：接口/适配器更多、调试链路更长、代码体积略增。
- 适用：多板/多 MCU、器件可替换、需要主机侧测试、长期维护。

### 抽象 Board（DIP 固定写法）

抽象 `Board` 放 `devices/board.hpp`，只暴露板级能力和 `devices`/`services` 接口。
完整最小模板见 `templates/devices/board.hpp`：

```cpp
class Board {
public:
	static Board &GetInstance();
	virtual bool Init() = 0;
	virtual Device *GetDevice() = 0;
	virtual DisplayPanel *GetDisplayPanel() = 0;

protected:
	Board() = default;
	~Board() = default;   // 静态对象，禁止 delete
};

#define IMPLEMENT_BOARD_CLASS(class_name) \
	Board *create_new_board() {              \
		static class_name instance;          \
		return &instance;                    \
	}
```

具体板只在 `bsp/<board>.cpp` 内定义，不提供 `.hpp`：

```cpp
namespace {

class Esp32SmartLockV01Board final : public Board {
public:
	bool Init() override;
	Device *GetDevice() override { return &device_; }
	DisplayPanel *GetDisplayPanel() override { return &panel_; }

private:
	Esp32UartDevice device_;
	Ssd1306Panel panel_;
};

}  // namespace

IMPLEMENT_BOARD_CLASS(Esp32SmartLockV01Board);
```

> DIP 模式下 `Application` 必须由 `main()` 注入 `Board &`（或 `BoardServices`）。
>
> 单例边界：`Board::GetInstance()` 只应出现在组合根 `main()`；为了主机侧测试，
> `Application` 应可构造并注入依赖，不要把 `GetInstance()` 写进业务逻辑。
>
> 本仓库模板取 Hybrid 组合根：`Application` 构造函数里
> `board_(Board::GetInstance())`；严格 DIP 改成 `Application(Board &board)` 由 `main()` 注入。

## 3. Hybrid（默认）

- 复杂/易变模块走接口：显示面板、传感器、指纹、人脸 → `devices`；
  网络、MQTT 传输、存储、时间、OTA → `services`。
- WiFi/MQTT 传输/存储/时间走 `services` 接口 + BSP 适配器；
  JSON 报文、命令分发、业务状态机留在 `app`，不要塞进 BSP。
- 简单稳定模块（GPIO 锁、蜂鸣器、LED）可以直接用 `bsp` 具体类。
- 必须在 `references/module-matrix.md` 中逐模块列出“走哪条路径”，禁止同一模块两条路径并存。
- **构建约束警告**：ESP-IDF 的组件依赖是组件级而非模块级。一旦 `app` `REQUIRES bsp`，
  app 就能 include bsp 的所有公共头文件，无法再按模块强制。真要加强制，必须让
  实现层不暴露公共头（头文件放 `src/` + `PRIV_INCLUDE_DIRS`），或把模块拆成独立组件。
  只在文档里写“逐模块”不算完成。

## 4. 选择判据（按优先级）

1. **板卡/器件变体**：≥ 2 个板卡/MCU/器件变体，或计划替换器件 → DIP/Hybrid。
2. **可测试性**：需要 mock/主机单测的模块 → DIP/Hybrid。
3. **复杂度**：≥ 3 个复杂或易变模块 → Hybrid/DIP。
4. **团队与生命周期**：多人并行、长期维护、频繁换代 → DIP/Hybrid。
5. **ROM/RAM**：只作为约束/否决项，不是主判据。
   - 极紧（例如 RAM < 32 KB 或 Flash < 128 KB）且无测试/多板需求 →
     Layered/Hybrid，避免接口爆炸。
   - 资源充足不等于必须 DIP；仍按 1~4 选择。
   - 用 `size` / `arm-none-eabi-size`、栈高水位实测，不要凭 MCU 型号猜。

| 场景 | 推荐 |
| --- | --- |
| 单板、单 MCU、固定器件、小团队、快速交付 | Layered |
| 多板/多 MCU、器件可替换、需要单测/mock、长期维护 | DIP |
| 复杂模块多，但简单器件更多 | **Hybrid（默认）** |
| ROM/RAM 极紧且无测试/多板需求 | Layered/Hybrid |

## 5. 用构建系统强制依赖

以 ESP-IDF 组件为例（其他构建系统用等价的可见性/依赖机制）：

Layered：

```cmake
idf_component_register(... REQUIRES bsp port)                    # app
idf_component_register(... REQUIRES port PRIV_REQUIRES devices)  # bsp
```

DIP：

```cmake
idf_component_register(... REQUIRES devices services port)          # app
idf_component_register(... REQUIRES devices services port)          # bsp
idf_component_register(... REQUIRES app bsp devices services port)  # main
```

Hybrid：

- 逐模块写清楚：抽象模块只出现在 `devices`/`services`；简单模块允许 `app` REQUIRE `bsp`。
  但 REQUIRE `bsp` 会暴露全部 BSP 公共头，只在实现层无公共头或模块独立成组件时才成立。
- 在 `references/module-matrix.md` 和平台契约里维护“模块 → 依赖路径”表，构建系统按表约束。

**验收方法**：临时在 `app` 里 include 一个不允许的层，构建必须失败；
如果仍能编译，说明依赖没有被真正约束。Hybrid 下如果 `app` 为了简单模块
`REQUIRES bsp`，这条验收只对该模块成立，对其他抽象模块不成立；请在
`references/module-matrix.md` 中记录例外。

## 6. 共同职责边界（三种模式都适用）

- `app/`：Application、各 APP 类（Display / Network / 采集 / 存储）、队列与任务、异步 API。
- `bsp/`：具体驱动、IO context、ISR、厂商回调 → 只投递。实现层默认不暴露公共头；
  驱动/板级头放 `src/`，用 `PRIV_INCLUDE_DIRS` 只对本组件可见。
- `devices/`：器件抽象与纯算法；不 include RTOS 与厂商头；不创建任务/队列。
- `services/`：平台能力接口；不 include RTOS 与厂商头；不写寄存器。
- `utility/`：滤波、格式化、数据结构等纯逻辑。
- `port/`：任务、队列、信号量、临界区、时间、调度器启动的薄封装。
- `app/`、`bsp/` 可以使用 RTOS API，但**平台差异只允许出现在 `port/`**；
  `#ifdef ESP_PLATFORM`、`#ifdef STM32...` 不应散落在 APP 代码里。

`Board` 形态：

- **Layered**：具体 `Board` 单例、私有构造、初始化一次，返回具体设备类。
- **DIP**：抽象 `Board` 只含 `Init()` 与 `GetXxx()`；具体 `<MCU><BoardName><Version>Board`
  只在 `bsp/<board>_board.cpp` 内定义，末尾用 `IMPLEMENT_BOARD_CLASS()` 注册，
  `main()` 注入 `Application`。
- **Hybrid**：抽象 `Board` 只返回被抽象的设备；简单器件可留在具体板或直接返回具体类。

APP 的依赖全部从构造函数注入，APP 内部不写 `Board::GetInstance().GetXxx()`。

## 7. 反模式

| 反模式 | 正确做法 |
| --- | --- |
| app 同时 include bsp 和 devices，且没有逐模块规则 | 选定 Layered/DIP/Hybrid，用 module-matrix 逐模块写清边界 |
| Hybrid 只在文档写“逐模块”，实现层仍暴露公共头 | 实现头放 `src/` + `PRIV_INCLUDE_DIRS`，或拆独立组件 |
| Board.hpp 暴露具体 bsp 类型，app 直接依赖驱动 | 暴露 devices/services 接口或 BoardServices |
| bsp include app（反向依赖） | app → bsp 或 app → devices ← bsp |
| devices include RTOS/厂商头 | devices 保持纯抽象，BSP 做适配 |
| 为每个 GPIO 都造接口 | 复杂/易变模块才抽象，简单器件可具体类 |
| 只在文档里写模式，没有用构建系统强制 | 用组件/模块依赖做编译期约束 |
| 抽象 `Board` 上出现板级专用方法（喂狗、板号、自检） | 留在具体板或抽成服务接口 |
| 具体板类仍叫 `Board`，与抽象类重名 | `<Mcu><BoardName><Version>Board` |
| 多个地方 `#ifdef` 选板 | 只编译一个 `bsp/*_board.cpp`，由链接期唯一符号选板 |
| 为具体板写 `.hpp` 并让 app include 它 | 具体板只在 `.cpp` 内定义，外部只见抽象 `Board` |
