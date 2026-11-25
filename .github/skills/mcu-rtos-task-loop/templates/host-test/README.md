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
- 测试只编译 `app` 的纯逻辑 + `devices` + `services` + `utility`，不链接 `bsp`/FreeRTOS。
- `app` 里直接调用 RTOS API 的部分（任务、队列）不参与主机测试：
  把状态机、协议、滤波抽成不碰 RTOS 的纯类，由 `TaskLoop` 调用；
  测试直接测这些纯类，用 `std::thread`/`std::mutex` 或直接同步调用驱动。
- 异步 API 的测试：断言“是否入队/是否返回 false”，最终状态由纯类同步验证，
  不等真实延时。
- HIL 测试另建目录，覆盖总线、传感器、WiFi/MQTT、OTA 和看门狗。

## CMake 骨架

```cmake
cmake_minimum_required(VERSION 3.16)
project(app_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(app_under_test
    ../components/utility/src/median_filter.cpp
    ../components/app/src/cloud_protocol.cpp
    ../components/app/src/report_policy.cpp
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

> 纯逻辑放 `utility/` 或 `app/` 内不碰 RTOS 的类；不要为了测试再造 `port` 的 host 实现。