idf_component_register(
    SRCS "src/rtos_port.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)
