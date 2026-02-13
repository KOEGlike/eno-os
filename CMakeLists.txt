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
  src/encoder.c
  src/audio.c
  src/audio_snippet.h
  src/screen.c
  src/sd.c
  lib/sd_card.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/lib
)
