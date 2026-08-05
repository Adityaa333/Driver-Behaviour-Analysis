idf_component_register(
    SRCS
        "main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        elm327_sim
        scenario_manager
        pid_generator
        nvs_flash
)
