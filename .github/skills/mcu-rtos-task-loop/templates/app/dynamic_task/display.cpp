#include "display.hpp"

#include <stdio.h>

bool Display::Init() {
	if (panel_ == nullptr) {
		return false;
	}
	if (queue_ != nullptr) {
		return false;
	}
	return port::CreateQueue(kQueueLength, sizeof(Message), &queue_);
}

bool Display::Start() {
	if (queue_ == nullptr || task_ != nullptr) {
		return false;
	}
	const port::TaskOptions options{"display", kTaskStackBytes, this,
		static_cast<int>(kPriority)};
	return port::CreateTask(&Display::TaskEntry, options, &task_);
}

bool Display::DrawText(int x, int y, const char *text) {
	if (text == nullptr) {
		return false;
	}
	Message message = {};
	message.event = Event::kDrawText;
	message.x = x;
	message.y = y;
	size_t i = 0;
	for (; i + 1 < kTextCapacity && text[i] != '\0'; ++i) {
		message.text[i] = text[i];
	}
	message.text[i] = '\0';
	return Post(message);
}

bool Display::DrawValue(int x, int y, int32_t value) {
	Message message = {};
	message.event = Event::kDrawValue;
	message.x = x;
	message.y = y;
	message.value = value;
	return Post(message);
}

bool Display::Clear() {
	Message message = {};
	message.event = Event::kClear;
	return Post(message);
}

void Display::TaskEntry(void *ctx) {
	static_cast<Display *>(ctx)->TaskLoop();
}

void Display::TaskLoop() {
	for (;;) {
		Message message = {};
		if (port::QueueReceive(queue_, &message, port::kWaitForever)) {
			Handle(message);
		}
	}
}

void Display::Handle(const Message &message) {
	switch (message.event) {
	case Event::kDrawText:
		HandleDrawText(message);
		break;
	case Event::kDrawValue:
		HandleDrawValue(message);
		break;
	case Event::kClear:
		HandleClear();
		break;
	default:
		break;
	}
}

void Display::HandleDrawText(const Message &message) {
	(void)panel_->ShowText(message.x, message.y, message.text);
}

void Display::HandleDrawValue(const Message &message) {
	char buffer[16] = {};
	snprintf(buffer, sizeof(buffer), "%ld", static_cast<long>(message.value));
	(void)panel_->ShowText(message.x, message.y, buffer);
}

void Display::HandleClear() {
	(void)panel_->Clear();
}

bool Display::Post(const Message &message) {
	if (!port::QueueSend(queue_, &message, port::kNoWait)) {
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	return true;
}
