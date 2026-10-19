# 回调上下文与资源生命周期

同一个函数，在不同上下文里能做的事情完全不同。生成代码前先判断上下文，再决定 API。

## 1. 上下文矩阵

| 上下文 | 典型来源 | 能否阻塞 | 能用什么 API | 该做什么 |
| --- | --- | --- | --- | --- |
| 硬件 ISR | EXTI、UART RX、GPIO、定时器中断 | 否 | `...FromISR` | 清标志、搬数据、投递、yield |
| 厂商事件任务 | ESP-IDF `esp_event`、WiFi、MQTT、SmartConfig | 不能久阻塞 | 非 FromISR | 只投递到 APP 队列 |
| 厂商回调（可能是 ISR） | STM32 HAL UART/EXTI、部分 SDK 回调 | 按文档 | 按上下文 | 只记录状态/投递 |
| RTOS 软件定时器 | FreeRTOS timer task | 不能阻塞 | 只投递 | 重活交给任务 |
| 厂商 timer | `esp_timer`（task/ISR）、协议栈 timer | 按 dispatch | 按上下文 | 只投递 |
| APP 任务 | 自己的 TaskLoop | 可以 | 全部 | 真正处理 |
| 主循环 handler | `MainEventLoop` | 不能久阻塞 | 非 FromISR | 更新状态 + 投递 |

> 不确定上下文时：查 SDK 文档/头文件。查不到就按“不能阻塞、只投递”处理。

## 2. 硬件 ISR

```cpp
void SensorReadyIsr(void *arg) {      // 按平台加 ISR 修饰符/链接属性
	auto *self = static_cast<Sensor *>(arg);
	bool woken = false;
	port::SemaphoreGiveFromIsr(self->ready_sem_, &woken);
	port::YieldFromIsr(woken);
}
```

允许：

- 清中断标志；
- 从寄存器/FIFO 搬少量数据；
- `port::QueueSendFromIsr` / `port::SemaphoreGiveFromIsr` / `port::TaskNotifyGiveFromIsr`；
- 处理 woken 并 yield。

禁止：

- 打印、`snprintf`、浮点格式化；
- 延时、忙等；
- 拿互斥量；
- 分配内存；
- 发起 I2C/SPI/Flash 事务；
- 解析长协议、写文件。

## 3. 厂商事件任务

以 ESP-IDF 为例：

- `esp_event` handler 默认运行在默认事件任务；
- WiFi/MQTT/SmartConfig 事件也在系统事件任务；
- 这些回调**不是 ISR**，不要用 `...FromISR`；
- 但也不能做重活：事件任务被阻塞会影响整个协议栈。

正确做法：

```cpp
static void WifiEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
	auto *app = static_cast<Application *>(arg);
	if (id == WIFI_EVENT_STA_DISCONNECTED) {
		app->PostWifiDisconnectedEvent();   // 只投递
	}
}
```

### 不要在事件回调里 stop/free

以下操作可能阻塞、与事件任务竞争或释放正在使用的资源：

- `esp_smartconfig_stop()`
- `esp_wifi_stop()` / `esp_wifi_deinit()`
- `esp_mqtt_client_stop()` / `esp_mqtt_client_destroy()`
- 关闭总线、删除任务/队列

应改为：

1. 回调里只更新状态、投递事件；
2. 由 APP 任务执行 stop/free；
3. 或在回调里启动一次性 timer，延迟到任务上下文执行。

## 4. 定时器回调

### FreeRTOS software timer

- 运行在 timer task；
- 不能阻塞、不能等队列、不能写 Flash；
- 只投递事件；
- 删除/停止定时器本身可以在回调里做（按 API 允许），但不要在这里释放其他资源。

### esp_timer

- `dispatch_method = ESP_TIMER_TASK`：运行在 esp_timer task；
- `dispatch_method = ESP_TIMER_ISR`：运行在 ISR，只能做 ISR 安全操作；
- 回调应短小；重活、stop/free 交给普通任务。

## 5. HAL 回调（STM32）

`HAL_UART_RxCpltCallback`、`HAL_GPIO_EXTI_Callback` 等通常运行在 ISR 上下文：

```cpp
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance != USART1) {
		return;
	}
	Board::GetInstance().GetSerial()->HandleRxCpltEvent();   // 只上报
}
```

驱动内部：

```cpp
void Serial::HandleRxCpltEvent() {
	bool woken = false;
	port::QueueSendFromIsr(rx_queue_, &byte_, &woken);
	port::YieldFromIsr(woken);
}
```

不要在 HAL 回调里解析协议、打印、延时、拿互斥量。

## 6. APP 任务

只有 APP 任务可以：

- 阻塞等待队列/信号量；
- 调用可能阻塞的驱动 API；
- 写 Flash/文件；
- 发起网络请求；
- 做协议解析、滤波、JSON 组包；
- 执行 stop/free/重新初始化。

## 7. 资源生命周期规则

1. 资源只在 init/Start 阶段创建；
2. 运行期不 create/delete；
3. 回调只“通知”，不“销毁”；
4. 需要销毁时投递到拥有该资源的任务；
5. 销毁前先停止生产者（ISR/定时器/协议栈回调）；
6. 删除任务/队列后，确保没有回调还引用它（置空句柄 + ready 标志）。

## 8. 快速判断

写回调前问自己：

- 这个回调是 ISR 还是任务？
- 如果我在里面阻塞 100ms，谁会受影响？
- 我调用的 stop/free 是否会与当前回调竞争？
- 我是否在 ISR 里用了非 FromISR API？
- 我是否在事件任务里做了本该交给 APP 任务的重活？

答不上来就先查文档，不要生成代码。
