# ESP-IDF 模板使用说明

## 复制位置

```text
<project>/
├── components/
│   ├── app/                        # Application + APP 类
│   ├── bsp/                        # 具体驱动 / services 实现
│   ├── devices/                    # 器件抽象（Board/Device 接口）
│   ├── services/                   # 网络/存储/时间/OTA 接口
│   └── port/
│       ├── CMakeLists.txt          <- templates/esp-idf/port/CMakeLists.txt
│       ├── include/port/rtos_port.hpp  <- templates/port/rtos_port.hpp
│       └── src/rtos_port.cpp       <- templates/esp-idf/port/rtos_port.cpp
├── main/
│   ├── CMakeLists.txt              <- main_CMakeLists.txt
│   ├── Kconfig.projbuild           <- Kconfig.projbuild
│   └── main.cpp                    <- main_app_main.cpp
├── partitions.csv                  <- partitions.csv
└── sdkconfig.defaults              <- sdkconfig.defaults
```

## 关键约定

- `port::TaskOptions.stack_bytes` 是**字节**；ESP-IDF 直接传给 `xTaskCreate`。
- `port::StartScheduler()` 是空实现；不要在 `app_main` 或 `Application::Start()`
  里调用 `vTaskStartScheduler()`。
- `port::QueueSendFromIsr` / `SemaphoreGiveFromIsr` 只返回 woken；
  ISR 末尾必须调用 `port::YieldFromIsr(woken)`。
- 组件必须 `REQUIRES freertos`，并使用 `#include "freertos/FreeRTOS.h"` 前缀。
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
- 启动日志确认没有重复启动调度器、没有栈溢出。
