#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "devices/display_panel.hpp"
#include "port/rtos_port.hpp"

// 静态版：外部 API 只投递事件，真正的绘制在 Display 任务里执行。
//
// DrawXxx() -> PostXxx() -> 内部队列 -> TaskLoop -> Handle(event) -> HandleXxx() -> panel
class Display {
public:
	explicit Display(DisplayPanel *panel) : panel_(panel) {}

	static constexpr uint32_t kTaskStackBytes = 3072;
	static constexpr uint32_t kQueueLength = 8;
	static constexpr uint8_t kPriority = 1;
	static constexpr size_t kTextCapacity = 32;

	bool Init();
	bool Start();

	// 异步：只做参数校验、拷贝、入队，返回是否入队。
	bool DrawText(int x, int y, const char *text);
	bool DrawValue(int x, int y, int32_t value);
	bool Clear();

	[[nodiscard]] uint32_t DroppedCount() const {
		return dropped_.load(std::memory_order_relaxed);
	}

private:
	enum class Event : uint32_t {
		kDrawText = 0,
		kDrawValue,
		kClear,
	};

	struct Message {
		Event event = Event::kDrawText;
		int x = 0;
		int y = 0;
		int32_t value = 0;
		char text[kTextCapacity] = {};
	};

	static void TaskEntry(void *ctx);
	void TaskLoop();
	void Handle(const Message &message);
	void HandleDrawText(const Message &message);
	void HandleDrawValue(const Message &message);
	void HandleClear();
	bool Post(const Message &message);

	DisplayPanel *panel_;
	port::StaticQueue<Message, kQueueLength> queue_;
	port::StaticTask<kTaskStackBytes> task_;
	std::atomic<uint32_t> dropped_{0};
};
