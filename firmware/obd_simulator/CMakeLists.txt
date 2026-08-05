cmake_minimum_required(VERSION 3.16)

# EXTRA_COMPONENT_DIRS not needed - components/ is an ESP-IDF default
# component search path, so elm327_sim, scenario_manager, and
# pid_generator are picked up automatically.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(obd_simulator)
