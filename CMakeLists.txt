cmake_minimum_required(VERSION 3.20.0)

if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)
endif()

# Allow Zephyr to discover custom boards in this repository (eno-os/boards/*).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Vendored SSD16XX display driver (see modules/display_ssd16xx/Kconfig).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/modules/display_ssd16xx)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(eno-os)

target_sources(app PRIVATE
  src/main.c
  src/buttons.c
  src/knob.c
  src/audio.c
  src/ui.c
  src/songs.c
  src/decoder.c
  lib/sd_card.c
  lib/helix/mp3dec.c
  lib/helix/mp3tabs.c
  lib/helix/real/bitstream.c
  lib/helix/real/buffers.c
  lib/helix/real/dct32.c
  lib/helix/real/dequant.c
  lib/helix/real/dqchan.c
  lib/helix/real/huffman.c
  lib/helix/real/hufftabs.c
  lib/helix/real/imdct.c
  lib/helix/real/polyphase.c
  lib/helix/real/scalfact.c
  lib/helix/real/stproc.c
  lib/helix/real/subband.c
  lib/helix/real/trigtabs.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/lib
  ${CMAKE_CURRENT_LIST_DIR}/lib/helix
  ${CMAKE_CURRENT_LIST_DIR}/lib/helix/pub
  ${CMAKE_CURRENT_LIST_DIR}/lib/helix/real
)

# Helix must decode faster than realtime; the app-wide -Og (debug
# optimizations) is too slow for 44.1 kHz stereo on the M33
set_source_files_properties(
  lib/helix/mp3dec.c
  lib/helix/mp3tabs.c
  lib/helix/real/bitstream.c
  lib/helix/real/buffers.c
  lib/helix/real/dct32.c
  lib/helix/real/dequant.c
  lib/helix/real/dqchan.c
  lib/helix/real/huffman.c
  lib/helix/real/hufftabs.c
  lib/helix/real/imdct.c
  lib/helix/real/polyphase.c
  lib/helix/real/scalfact.c
  lib/helix/real/stproc.c
  lib/helix/real/subband.c
  lib/helix/real/trigtabs.c
  PROPERTIES COMPILE_OPTIONS "-O2"
)
