/* 仅展示启动顺序，外设初始化按 CubeMX 生成代码补全。 */

#include "FreeRTOS.h"
#include "task.h"

#include "app/application.hpp"
#include "bsp/board.hpp"

int main(void) {
	HAL_Init();
	SystemClock_Config();

	MX_GPIO_Init();
	MX_USART1_UART_Init();
	// ... 其他外设初始化 ...

	// 单例在调度器启动前构造
	if (!Board_Init()) {
		Error_Handler();
	}
	Application_Start();   // 只创建任务，不启动调度器

	vTaskStartScheduler(); // 只调用一次
	for (;;) {
		// 返回说明任务创建失败/内存不足
	}
}
