#pragma once

// 显示面板抽象：只放纯接口，不 include RTOS / 厂商头。
// 真实工程放在 devices/，实现放在 bsp/。
class DisplayPanel {
public:
	virtual ~DisplayPanel() = default;
	virtual bool ShowText(int x, int y, const char *text) = 0;
	virtual bool Clear() = 0;
};
