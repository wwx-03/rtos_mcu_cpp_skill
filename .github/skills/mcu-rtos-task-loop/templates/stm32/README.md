# STM32 / CubeMX 模板使用说明

## 复制位置

```text
<project>/
├── port/
│   ├── include/port/rtos_port.hpp   <- templates/port/rtos_port.hpp
│   └── src/rtos_port.cpp            <- templates/stm32/port/rtos_port.cpp
├── Core/Src/main.c                  <- 参考 main_template.cpp 的启动顺序（CubeMX 生成文件保留时用 extern "C" 包装）
└── Core/Inc/FreeRTOSConfig.h        <- 参考 freertos_config_notes.md
```

## 关键约定

- `port::TaskOptions.stack_bytes` 是**字节**；STM32 实现里向上取整除以
  `sizeof(StackType_t)` 转成 word。
- 模板是动态创建版；`configSUPPORT_DYNAMIC_ALLOCATION = 0` 时需替换为 Static API。
- `port::StartScheduler()` 调用 `vTaskStartScheduler()`；只在 `main()` 里调用一次。
- HAL 时基改用 TIM，SysTick/PendSV/SVC 交给 FreeRTOS。
- 调用 `...FromISR` 的中断，其 NVIC 抢占优先级数值要满足
  `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`。
- HAL UART/EXTI 回调通常运行在 ISR 上下文：只投递，不解析/不打印/不阻塞。

## 验证

- `arm-none-eabi-nm` 确认 SysTick/PendSV/SVC 只有一份定义；
- 静态分配工程不引入 `malloc/free`；
- 启动日志无栈溢出、无 HardFault；
- 条件编译分支分别构建通过。
