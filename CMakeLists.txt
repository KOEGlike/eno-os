cmake_minimum_required(VERSION 3.20.0)

if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)
endif()

# Allow Zephyr to discover custom boards in this repository (eno-os/boards/*).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(eno-os)

target_sources(app PRIVATE 
  src/main.c
  src/sd_card.c
  src/audio_snippet.h
)
