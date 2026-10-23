#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "display.hpp"
#include "port/rtos_port.hpp"

class Board;

// 静态版：队列/任务/定时器存储都在对象内，不依赖 heap。
// GetInstance() 的函数内 static 已保证对象在静态存储区；
// 不要把 Application 放到任务栈上或按值传递。
class Application {
public:
	static Application &GetInstance() {
		static Application instance;
		return instance;
	}

	bool Init();
	bool Start();

	[[nodiscard]] uint32_t DroppedCount() const {
		return dropped_.load(std::memory_order_relaxed);
	}

private:
	// 内部事件类型；App 层有事件的类都应该私有化。
	struct Event {
		enum class Type : uint32_t {
			kStart = 0,
			kTimerElapsed,
			kDeviceReceived,
		};

		Type type = Type::kStart;

		union {
			struct {
				size_t size;
			} device_received;
		} data = {};
	};

	Application();
	Application(const Application &) = delete;
	Application &operator=(const Application &) = delete;

	// 任务/队列/定时器预算；与平台契约、模块矩阵保持一致。
	static constexpr uint32_t kTaskStackBytes = 4096;
	static constexpr uint32_t kQueueLength = 16;
	static constexpr uint32_t kTimerPeriodMs = 1000;
	static constexpr uint8_t kPriority = 4;
	static constexpr size_t kMaxDeviceChunk = 64;

	static void TimerEntry(void *ctx);
	static void TaskEntry(void *ctx);

	void MainEventLoop();
	void HandleEvent(const Event &event);
	void HandleStart();
	void HandleTimerElapsed();
	void HandleDeviceReceived(size_t size);

	bool Post(const Event &event);
	bool PostStart();
	bool PostTimerElapsed();
	bool PostDeviceReceived(size_t size);
	// ISR 专用：调用后必须 YieldFromIsr(woken)。
	bool PostDeviceReceivedFromIsr(size_t size, port::IsrWoken &woken);

	Board &board_;
	Display display_;
	port::StaticQueue<Event, kQueueLength> queue_;
	port::StaticTask<kTaskStackBytes> task_;
	port::StaticTimer timer_;
	uint32_t uptime_seconds_ = 0;
	std::atomic<uint32_t> dropped_{0};
};