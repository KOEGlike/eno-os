cmake_minimum_required(VERSION 3.20.0)

if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)
endif()

# Allow Zephyr to discover custom boards in this repository (eno-os/boards/*).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Include custom devicetree bindings from the tad5x12 module.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../modules/lib/tad5x12)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(eno-os)

target_sources(app PRIVATE 
  src/main.c
  src/sd_card.c
)
