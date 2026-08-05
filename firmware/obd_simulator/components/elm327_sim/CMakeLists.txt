idf_component_register(
    SRCS
        "elm327_sim.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        bt
        pid_generator
        log
        esp_hw_support
)
