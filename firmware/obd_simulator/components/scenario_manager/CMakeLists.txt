idf_component_register(
    SRCS
        "scenario_manager.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        pid_generator
        log
        esp_timer
        esp_hw_support
        driver
)

