#include <stdlib.h>

#include "app/application.hpp"
#include "devices/board.hpp"
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

	Application &app = Application::GetInstance();
	if (!app.Init()) {
		ESP_LOGE(TAG, "app init failed");
		abort();
	}
	if (!app.Start()) {
		ESP_LOGE(TAG, "app start failed");
		abort();
	}
}
