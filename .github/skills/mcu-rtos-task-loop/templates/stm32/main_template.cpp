/* 仅展示启动顺序，外设初始化按 CubeMX 生成代码补全。
 * 若必须保留 CubeMX 生成的 main.c，在其中调用 extern "C" 的 AppInit()/AppStart()
 * 包装函数，包装内部再调用下面的 C++ 生命周期。 */

#include "FreeRTOS.h"
#include "task.h"

#include "app/application.hpp"
#include "devices/board.hpp"
#include "main.h"

int main(void) {
	HAL_Init();
	SystemClock_Config();

	MX_GPIO_Init();
	MX_USART1_UART_Init();
	// ... 其他外设初始化 ...

	// 单例在调度器启动前构造
	if (!Board::GetInstance().Init()) {
		Error_Handler();
	}
	Application &app = Application::GetInstance();
	if (!app.Init()) {
		Error_Handler();
	}
	if (!app.Start()) {     // 只创建任务，不启动调度器
		Error_Handler();
	}

	vTaskStartScheduler();  // 只调用一次
	portDISABLE_INTERRUPTS();
	for (;;) {
		// 返回说明任务创建失败/内存不足
	}
}
