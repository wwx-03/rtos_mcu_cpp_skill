# 案例：水质传感器 → 显示 → MQTT

这是一个按本技能落地的完整案例（ESP32-C3 + ESP-IDF 6）。它不是模板代码，而是用来展示
“分层、事件流、平台契约、失败降级”如何组合。

## 1. 平台契约

```markdown
- MCU：ESP32-C3（单核 RISC-V）
- RTOS：ESP-IDF FreeRTOS
- SDK：ESP-IDF 6.x
- 入口：app_main()，调度器已启动
- 任务栈单位：byte
- 栈高水位单位：byte
- 分配：init 阶段动态创建，检查返回值
- ISR：GPIO RDY 下降沿，xSemaphoreGiveFromISR
- 临界区：单核关中断（C3）
- 回调：WiFi/MQTT/SmartConfig 均为系统事件任务
- 构建：idf_component_register + REQUIRES freertos
- Flash：2MB，自定义分区，app 1.9MB
```

## 2. 分层

```text
components/
├── app/
│   ├── application.*     # MainEventLoop、主队列
│   ├── water_monitor.*   # 周期采集 + 滤波 + 有效性
│   ├── display.*         # OLED 渲染任务
│   └── network.*         # WiFi/MQTT 状态与上报
├── bsp/
│   ├── board.*           # 单例、I2C 总线、依赖组装
│   ├── ads1115_i2c.*     # RDY ISR + 单次转换 + OS 位兜底
│   ├── ssd1306_panel.*   # esp_lcd SSD1306
│   ├── wifi_station.*    # SmartConfig + 重连
│   └── onenet_mqtt.*     # token + 发布
├── devices/              # pH/浊度算法、ADC/显示抽象
├── utility/              # 中值滤波、定点格式化
└── port/                 # 统一入口/栈单位/队列/临界区
```

## 3. 数据流

```text
ADS1115 RDY ISR
    │  SemaphoreGiveFromISR
    ▼
WaterMonitor task
    │  3 次采样 + 中值 + 5 点滤波 + 工程量换算
    ▼
Application::PostWaterQualityEvent
    ▼
MainEventLoop
    ├─► Display::ShowWaterQuality（异步）
    └─► Network::Publish（异步）
             │
             ▼
        MQTT property post
```

## 4. 关键决策

| 决策 | 选择 | 原因 |
| --- | --- | --- |
| 采样周期 | 2s | 水质变化慢，降低总线/功耗 |
| 滤波 | 3 次中值 + 5 点中值 | 抑制脉冲噪声，固定 RAM |
| 无效采样 | 显示 `--`，不上报 | 避免把断线当成 0 |
| 离线数据 | 直接丢弃 | 数据有时效性，避免无限缓存 |
| MQTT | QoS 1 | 兼顾可靠性与内存 |
| 配网 | SmartConfig | 无按键/屏幕输入 |
| 栈 | 4~6KB | 日志 + snprintf + 浮点 |

## 5. 真实踩坑

1. **ESP-IDF 里重复调用 `vTaskStartScheduler()`**  
   `app_main` 已在调度器中；`Application::Start()` 只建任务。
2. **栈大小按 word 传**  
   ESP-IDF 是 byte；统一用 `port::TaskOptions.stack_bytes`。
3. **在 SmartConfig 事件回调里 `esp_smartconfig_stop()`**  
   改为投递任务或一次性 timer 延迟。
4. **ADS1115 3.3V 供电 + 浊度 3.8V 输出**  
   必须分压或改 5V 供电 + 电平转换；这是硬件契约，不是软件能修的。
5. **初始化失败直接 abort**  
   改为 `ready_` + 降级，便于板上分步排查。

## 6. 可复用结论

- `Application` 只负责派发，不产生业务逻辑；
- APP 之间只通过异步 API 交互；
- 平台差异集中在 `port/`；
- 回调只投递，重活交给任务；
- 任何“未知语义”都先查 SDK，再写代码。
