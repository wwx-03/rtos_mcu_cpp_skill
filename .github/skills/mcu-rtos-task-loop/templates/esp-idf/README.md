# ESP-IDF 模板使用说明

## 复制位置

```text
<project>/
├── components/
│   ├── app/                        # Application + APP 类（可用 freertos）
│   ├── bsp/                        # 具体驱动 / services 实现（可用 freertos + esp_*）
│   ├── devices/                    # 器件抽象（Board/Device 接口，不含 RTOS/IDF 头）
│   └── services/                   # 网络/存储/时间/OTA 接口（不含 RTOS/IDF 头）
├── main/
│   ├── CMakeLists.txt              <- main_CMakeLists.txt
│   ├── Kconfig.projbuild           <- Kconfig.projbuild
│   └── main.cpp                    <- main_app_main.cpp
├── partitions.csv                  <- partitions.csv
└── sdkconfig.defaults              <- sdkconfig.defaults
```

**没有 `port/` 组件**：`app/`、`bsp/` 直接 include `freertos/FreeRTOS.h` 等原生头文件。

## 关键约定

- 任务栈单位是 **byte**；`xTaskCreate` 直接收字节数，预算用 `kTaskStackBytes`。
- 队列/任务/定时器在 `Init()`/`Start()` 里一次创建，运行期不 create/delete/free。
- 不要在 `app_main` 或 `Application::Start()` 里调用 `vTaskStartScheduler()`。
- ISR 用 `xQueueSendFromISR` / `xSemaphoreGiveFromISR` / `vTaskNotifyGiveFromISR`，
  末尾 `portYIELD_FROM_ISR(woken)`。
- 组件 `REQUIRES freertos`，并使用 `#include "freertos/FreeRTOS.h"` 前缀。
- ESP-IDF 没有 `configTOTAL_HEAP_SIZE`：内部 RAM 由 IDF 堆统一管理；
  大缓冲用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` 显式放 PSRAM。
- ESP-MQTT 是托管组件 `espressif/mqtt`（IDF 6+），写在 `main/idf_component.yml`。
- `sdkconfig.defaults` 固定了 `CONFIG_IDF_TARGET`，避免无 sdkconfig 时构建成 esp32。

## 验证

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
idf.py fullclean
idf.py build
idf.py size
```

- 从无 `sdkconfig` 构建，确认目标仍是 esp32c3；
- `idf.py size` 检查 app 分区余量；
- 启动日志确认没有重复启动调度器、没有栈溢出；
- 启动后用 `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`
  确认内部 RAM 水位稳定。