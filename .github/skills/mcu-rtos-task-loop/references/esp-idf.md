# ESP-IDF 适配参考

适用于 ESP-IDF 5.x / 6.x。**API 与构建系统会随版本变化，使用前先看当前 SDK 的头文件与迁移指南。**

## 1. 启动模型

```cpp
extern "C" void app_main(void) {
    // app_main 已经运行在调度器创建的 main task 中
    if (!Board::GetInstance().Init()) {
        abort();
    }
    Application &app = Application::GetInstance();
    if (!app.Init()) {              // 建队列/信号量/定时器
        abort();
    }
    app.Start();                    // 只创建任务，不启动调度器
}
```

- **不要**在 `app_main` 或 `Application::Start()` 里调用 `vTaskStartScheduler()`；
  否则会触发 FreeRTOS 断言/未定义行为。
- `Application::Start()` 只创建任务/启动 timer 并返回；`app_main` 返回后调度器继续运行。
- `Board` / `Application` 单例可在 `app_main` 中构造；它们已经在任务上下文，
  不需要“调度器启动前”这条 CubeMX 规则。

## 2. 任务栈单位

ESP-IDF 的 `xTaskCreate()` / `xTaskCreateStatic()` 栈深参数是 **byte**：

```c
xTaskCreate(entry, "task", 4096, arg, 4, &handle);  // 4096 bytes
```

`uxTaskGetStackHighWaterMark()` 也返回 **byte**。代码里统一使用
`port::TaskOptions.stack_bytes`，平台实现直接传给 `xTaskCreate`。

`templates/esp-idf/port/rtos_port.cpp` 还提供 `CreateTimer/Start/Stop/Reset/DeleteTimer`
以及 `GetStackHighWaterMark`、`GetFreeHeap`、`GetMinimumEverFreeHeap`。
静态 task/queue 用 `port::StaticTask` / `port::StaticQueue`（见 `templates/port/rtos_port.hpp`），
完整静态/动态示例见 `templates/app/`。
定时器回调通过 trampoline 转成 `TimerEntry(void*)`，运行在 FreeRTOS timer task，
只能投递，不能阻塞或 stop/free 其他资源。

## 3. 组件与构建

### 组件结构

```text
project/
├── components/
│   ├── app/        # Application + APP 类
│   ├── bsp/        # Board、驱动、services 实现
│   ├── devices/    # 器件抽象（不含 RTOS/IDF 头）
│   ├── services/   # 网络/存储/时间/OTA 等平台能力接口
│   ├── utility/    # 纯逻辑
│   └── port/       # 唯一平台依赖点
├── main/           # app_main、Kconfig.projbuild、idf_component.yml
├── partitions.csv
└── sdkconfig.defaults
```

### CMake 依赖（按架构模式）

具体选哪种由 `references/architecture.md` 决定。

Layered（app 用 bsp，devices 只在 bsp 内部可见）：

```cmake
idf_component_register(
    SRCS "src/application.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bsp port
)
# bsp: REQUIRES port PRIV_REQUIRES devices
```

DIP（app 只看到 devices/services，bsp 实现接口，main 做组合根）：

```cmake
idf_component_register(
    SRCS "src/application.cpp"
    INCLUDE_DIRS "include"
    REQUIRES devices services port
)
# bsp:  REQUIRES devices services port
# main: REQUIRES app bsp devices services port
```

DIP 的 Board 与选板：

- 抽象 `Board`、`IMPLEMENT_BOARD_CLASS`、具体板只在 `.cpp` 内定义：
  见 `references/architecture.md` 与 `templates/devices/board.hpp`。
- ESP-IDF 选板 = 只把一块板的 `.cpp` 加入 `SRCS`；多块重复符号，缺失 undefined reference。
- 组合根顺序：

```cpp
Board::GetInstance().Init();
Application &app = Application::GetInstance();
if (!app.Init()) { abort(); }
if (!app.Start()) { abort(); }
```

> 模板的 `Application` 在构造函数里取 `Board::GetInstance()`（组合根）；
> 严格 DIP 可改为构造注入 `Board &`，测试时注入假 Board。

验收：在 app 里 include 一个不允许的层，构建必须失败。

IDF 6 中 `#include "FreeRTOS.h"` 不再默认可用，必须：

- 使用 `#include "freertos/FreeRTOS.h"`、`"freertos/queue.h"`、`"freertos/task.h"`；
- 在 `REQUIRES` 中显式加入 `freertos`。

### 托管组件

IDF 6 起 ESP-MQTT 已移出核心，使用托管组件：

```yaml
# main/idf_component.yml
dependencies:
  espressif/mqtt: "^1.1.0"
```

注意组件名是 `espressif/mqtt`，不是 `espressif/esp-mqtt`。
首次构建需要联网下载，`managed_components/` 不应入库。

### 目标、分区与 sdkconfig

`sdkconfig` 常被 gitignore。建议在 `sdkconfig.defaults` 固定目标与分区：

```ini
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_FREERTOS_HZ=100
```

2 MB flash 的示例分区：

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x1F0000,
```

WiFi + MQTT + TLS 很容易超过 1 MB，默认单 app 分区可能不够；先看 `idf.py size`。

## 4. 回调上下文

| 回调/事件 | 上下文 | 规则 |
| --- | --- | --- |
| `app_main` | main task | 可创建任务；不要启动调度器 |
| `esp_event` handler | 默认事件任务 | 不阻塞；只投递 |
| WiFi 事件 | 默认事件任务 | 不 stop/free；只投递 |
| MQTT 事件 | MQTT 客户端任务 | 不阻塞；只投递 |
| SmartConfig 事件 | 默认事件任务 | **不要在回调里 `esp_smartconfig_stop()`** |
| `esp_timer`（dispatch=task） | esp_timer task | 不阻塞；只投递 |
| `esp_timer`（dispatch=ISR） | ISR | 只做 ISR 安全操作 |
| GPIO ISR | ISR | 用 `xQueueSendFromISR` / `xSemaphoreGiveFromISR` |
| UART 事件 | 通常为 ISR/驱动任务 | 先查驱动文档 |

需要停止配网/关闭协议栈时，把动作投递到任务，或用一次性 `esp_timer`
延迟到任务上下文执行。

## 5. 中断与 GPIO ISR

```cpp
void IRAM_ATTR SensorReadyIsr(void *arg) {
    auto *self = static_cast<Sensor *>(arg);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->ready_sem_, &woken);
    portYIELD_FROM_ISR(woken);
}
```

- ISR 处理函数与它调用的代码要满足 IRAM 安全要求（按 IDF 配置）；
- `gpio_install_isr_service()` 可重复调用时返回 `ESP_ERR_INVALID_STATE`，视为已安装；
- 中断优先级由 GPIO 驱动/IDF 管理；不要套用 Cortex-M 的 NVIC 数值规则。

## 6. 双核与临界区（ESP32 / ESP32-S3）

- 通信/协议/采集任务用 `xTaskCreatePinnedToCore` 明确亲和性；
- ESP-IDF 6 的 `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` 需要
  `portMUX_TYPE` 参数（单核也一样）：

  ```c
  static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  taskENTER_CRITICAL(&lock);
  shared_counter++;
  taskEXIT_CRITICAL(&lock);
  ```

- 双核共享数据必须用 spinlock（`portMUX_TYPE`）或原子操作；
  `taskENTER_CRITICAL` 只关当前核中断，不能保护另一个核；
- 互斥量仍然只能在任务里获取；
- ISR 里不要做跨核阻塞操作；ISR 临界区用 `..._FROM_ISR` / `..._ISR` 变体。

## 7. I2C / SPI

IDF 6 使用新的 `driver/i2c_master.h`：

- `i2c_new_master_bus()` → `i2c_master_bus_add_device()`；
- `i2c_master_transmit()` / `i2c_master_transmit_receive()`；
- 多设备共享总线由驱动串行化；仍建议给每个事务设置超时；
- 不要在 ISR 里发起 I2C/SPI 事务；
- 启动时 `i2c_master_probe()` 或扫描地址，确认器件存在。

SSD1306 可直接使用 IDF 内置 `esp_lcd`：

- `esp_lcd_new_panel_io_i2c()`；
- `esp_lcd_new_panel_ssd1306()`；
- 1bpp framebuffer，`esp_lcd_panel_draw_bitmap()`。

## 8. 常见错误（真实项目踩过）

| 错误 | 现象 | 修正 |
| --- | --- | --- |
| `Application::Start()` 再调 `vTaskStartScheduler()` | 启动断言/挂死 | 只建任务；调度器已由 IDF 启动 |
| 栈大小按 word 传 | 实际栈只有 1/4，随机溢出 | 按 byte 传，`port` API 用 `stack_bytes` |
| `#include "FreeRTOS.h"` | 编译时报找不到头文件 | `#include "freertos/FreeRTOS.h"` + `REQUIRES freertos` |
| 依赖写成 `espressif/esp-mqtt` | 组件下载 404 | 用 `espressif/mqtt` |
| 在 SmartConfig 事件里 stop | 可能与事件任务竞争 | 投递任务或一次性 timer 延迟 |
| 在 WiFi/MQTT 回调里做重活 | 协议栈卡顿/看门狗 | 只投递到 APP 队列 |
| 默认分区表 | app 放不下 | 自定义分区 + 2MB/4MB flash |
| 无 sdkconfig 构建成 esp32 | 烧录目标错误 | `sdkconfig.defaults` 固定 `CONFIG_IDF_TARGET` |

## 9. 验证命令

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
idf.py build
idf.py size
idf.py -p COMx flash monitor
```

检查：

- `idf.py build` 从无 `sdkconfig` 开始也能选对目标；
- `idf.py size` 的 app 分区余量；
- 启动日志中任务创建成功、无看门狗/栈溢出；
- 条件编译分支（例如强制配网、显示翻转）分别构建通过。
