#pragma once

#include <stddef.h>
#include <utility>

// 器件抽象：只放纯接口，不 include RTOS / 厂商头。
// 真实工程放在 devices/，实现放在 bsp/。
class Device {
public:
	using ReceiveCallback = void (*)(size_t size, void *ctx);

	virtual ~Device() = default;
	virtual size_t Write(const void *data, size_t size) = 0;
	virtual size_t Read(void *data, size_t size) = 0;

	void SetReceivedCallback(ReceiveCallback cb, void *ctx) {
		received_entry_ = {cb, ctx};
	}

protected:
	// 在任务上下文调用；ISR 里先投递到 BSP 任务，再由 BSP 任务调用。
	void NotifyReceived(size_t size) {
		if (received_entry_.first != nullptr) {
			received_entry_.first(size, received_entry_.second);
		}
	}

	std::pair<ReceiveCallback, void *> received_entry_;
};
