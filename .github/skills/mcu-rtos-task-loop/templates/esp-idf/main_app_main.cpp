#include <stdlib.h>

#include "app/application.hpp"
#include "bsp/board.hpp"
#include "esp_log.h"

namespace {
const char *TAG = "main";
}

extern "C" void app_main(void) {
	ESP_LOGI(TAG, "booting");

	// ESP-IDF 的 app_main 已经运行在调度器任务中。
	if (!Board::GetInstance().Init()) {
		ESP_LOGE(TAG, "board init failed");
		abort();
	}

	Application::GetInstance().Start();  // 只创建任务，不启动调度器
}
