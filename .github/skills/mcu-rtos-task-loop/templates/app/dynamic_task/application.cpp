#include "application.hpp"

#include "devices/board.hpp"
#include "devices/device.hpp"

Application::Application()
	: board_(Board::GetInstance()), display_(board_.GetDisplayPanel()) {}

bool Application::Init() {
	if (queue_ != nullptr) {
		return false;
	}
	// 屏幕是可选外设时可改为降级；这里示例 fail-fast。
	if (!display_.Init()) {
		return false;
	}
	if (!port::CreateQueue(kQueueLength, sizeof(Event), &queue_)) {
		return false;
	}
	const port::TimerOptions options{"App Tmr", kTimerPeriodMs, true, this};
	if (!port::CreateTimer(&Application::TimerEntry, options, &timer_)) {
		return false;
	}
	return true;
}

bool Application::Start() {
	Device *device = board_.GetDevice();
	if (device != nullptr) {
		// 回调必须在任务上下文调用；ISR 里先投递到 BSP 任务，再由 BSP 任务调用。
		device->SetReceivedCallback(+[](size_t size, void *ctx) {
			(void)static_cast<Application *>(ctx)->PostDeviceReceived(size);
		}, this);
	}
	if (queue_ == nullptr || task_ != nullptr || timer_ == nullptr) {
		return false;
	}
	if (!display_.Start()) {
		return false;
	}

	const port::TaskOptions options{"app_main", kTaskStackBytes, this,
		static_cast<int>(kPriority)};
	if (!port::CreateTask(&Application::TaskEntry, options, &task_)) {
		return false;
	}
	if (!port::StartTimer(timer_, port::kNoWait)) {
		return false;
	}
	return PostStart();
}

bool Application::PostStart() {
	Event event = {};
	event.type = Event::Type::kStart;
	return Post(event);
}

bool Application::PostTimerElapsed() {
	Event event = {};
	event.type = Event::Type::kTimerElapsed;
	return Post(event);
}

bool Application::PostDeviceReceived(size_t size) {
	Event event = {};
	event.type = Event::Type::kDeviceReceived;
	event.data.device_received.size = size;
	return Post(event);
}

bool Application::PostDeviceReceivedFromIsr(size_t size, port::IsrWoken &woken) {
	Event event = {};
	event.type = Event::Type::kDeviceReceived;
	event.data.device_received.size = size;
	if (!port::QueueSendFromIsr(queue_, &event, &woken.value)) {
		// 32 位原子在 Cortex-M/ESP32 上 lock-free；其他平台需确认 ISR 安全性。
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	return true;
}

void Application::TimerEntry(void *ctx) {
	static_cast<Application *>(ctx)->PostTimerElapsed();
}

void Application::TaskEntry(void *ctx) {
	static_cast<Application *>(ctx)->MainEventLoop();
}

void Application::MainEventLoop() {
	for (;;) {
		Event event = {};
		if (port::QueueReceive(queue_, &event, port::kWaitForever)) {
			HandleEvent(event);
		}
	}
}

void Application::HandleEvent(const Event &event) {
	switch (event.type) {
	case Event::Type::kStart:
		HandleStart();
		break;
	case Event::Type::kTimerElapsed:
		HandleTimerElapsed();
		break;
	case Event::Type::kDeviceReceived:
		HandleDeviceReceived(event.data.device_received.size);
		break;
	default:
		break;
	}
}

void Application::HandleStart() {
	// 在这里启动各 APP：display_.Start(); network_.Start(); ...
	// 每个 APP 的 Init()/Start() 失败都要记录并决定降级还是停机。
	(void)display_.DrawText(0, 0, "ready");
}

void Application::HandleTimerElapsed() {
	// 周期事件：只更新状态 + 投递，不做重活。
	++uptime_seconds_;
	(void)display_.DrawValue(0, 16, static_cast<int32_t>(uptime_seconds_));
}

void Application::HandleDeviceReceived(size_t size) {
	Device *device = board_.GetDevice();
	if (device == nullptr) {
		return;
	}

	uint8_t buffer[kMaxDeviceChunk] = {};
	const size_t read_size = size < kMaxDeviceChunk ? size : kMaxDeviceChunk;
	const size_t read = device->Read(buffer, read_size);

	// 把 buffer 中的数据投递给对应 APP；不要在 MainEventLoop 里做协议解析。
	(void)display_.DrawValue(0, 32, static_cast<int32_t>(read));
}

bool Application::Post(const Event &event) {
	if (!port::QueueSend(queue_, &event, port::kNoWait)) {
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	return true;
}