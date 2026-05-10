cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_t_display_ads1256)

target_sources(app PRIVATE
    src/main.cpp
    src/app/app_controller.cpp
    src/app/app_events.cpp
    src/app/app_state.cpp
    src/drivers/ads1256.cpp
    src/services/acquisition_service.cpp
    src/services/ble_service.cpp
    src/services/signal_processing_service.cpp
    src/ui/ui.cpp
    src/ui/ui_events.cpp
)

target_include_directories(app PRIVATE
    include
    src
)
