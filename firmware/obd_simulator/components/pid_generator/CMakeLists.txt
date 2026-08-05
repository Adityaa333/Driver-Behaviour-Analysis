idf_component_register(
    SRCS
        "pid_generator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        log
        esp_timer
        esp_hw_support
)

