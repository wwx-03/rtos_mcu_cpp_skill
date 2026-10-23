# 主机侧测试模板（DIP / Hybrid）

目标：让 `app` 的协议、状态机、算法在 PC 上运行，不依赖 RTOS、厂商 SDK 和真实硬件。

## 目录建议

```text
tests/
├── CMakeLists.txt
├── mocks/
│   ├── fake_board.hpp
│   ├── fake_network.hpp
│   ├── fake_protocol.hpp
│   └── fake_store.hpp
└── app/
    ├── test_cloud_protocol.cpp
    └── test_state_machine.cpp
```

## 原则

- `devices`/`services` 是纯接口，不 include RTOS/厂商头；mock 直接实现这些接口。
- `app` 通过构造函数注入依赖，不调用 `Board::GetInstance()`。
- 测试只编译 `app` + `devices` + `services` + `utility`，不链接 `bsp`/FreeRTOS。
- 对时间、队列、任务通知做薄封装时，测试用假实现替换 `port`，或把纯逻辑抽到
  `utility` 做单元测试。
- 异步 API 的测试：等待 mock 队列/条件变量，断言“是否入队”和最终状态，不等真实延时。
- HIL 测试另建目录，覆盖总线、传感器、WiFi/MQTT、OTA 和看门狗。

## CMake 骨架

```cmake
cmake_minimum_required(VERSION 3.16)
project(app_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(app_under_test
    ../components/app/src/cloud_protocol.cpp
    ../components/app/src/relay_manager.cpp
    ../components/app/src/sensor_manager.cpp
)
target_include_directories(app_under_test PUBLIC
    ../components/app/include
    ../components/devices/include
    ../components/services/include
    ../components/utility/include
)

add_executable(app_tests app/test_cloud_protocol.cpp mocks/fake_board.cpp)
target_link_libraries(app_tests PRIVATE app_under_test)
```

> 如果 `app` 依赖 `port`，给测试提供 `port` 的 host 实现（`std::thread`/`std::mutex`/
> 条件变量），不要让测试链接 ESP-IDF/FreeRTOS。
