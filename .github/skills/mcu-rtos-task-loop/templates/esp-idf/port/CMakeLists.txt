idf_component_register(
    SRCS "rtos_port.cpp" "src/rtos_port.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)
