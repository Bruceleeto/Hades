cmake_minimum_required(VERSION 3.15)
project(Hades C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 11)

# Include directories
include_directories(include source)

# Compiler flags
add_compile_options(
    -g 
    -pg              
    -fms-extensions
    -Wno-microsoft-anon-tag
)

add_link_options(-Wl,-Map=output.map -pg)

add_compile_definitions(HADES_VERSION="1.0.0")

# Find dependencies
find_package(SDL2 REQUIRED)
find_library(MATH_LIBRARY m)

###############################
## GBA Core Library
###############################

set(LIBGBA_SOURCES
    source/gba/apu/apu.c
    source/gba/apu/fifo.c
    source/gba/apu/modules.c
    source/gba/apu/noise.c
    source/gba/apu/tone.c
    source/gba/apu/wave.c
    source/gba/core/arm/alu.c
    source/gba/core/arm/bdt.c
    source/gba/core/arm/branch.c
    source/gba/core/arm/sdt.c
    source/gba/core/arm/core.c
    source/gba/core/arm/mul.c
    source/gba/core/arm/psr.c
    source/gba/core/arm/swi.c
    source/gba/core/arm/swp.c
    source/gba/core/thumb/alu.c
    source/gba/core/thumb/bdt.c
    source/gba/core/thumb/branch.c
    source/gba/core/thumb/core.c
    source/gba/core/thumb/logical.c
    source/gba/core/thumb/sdt.c
    source/gba/core/thumb/swi.c
    source/gba/core/core.c
    source/gba/gpio/gpio.c
    source/gba/gpio/rtc.c
    source/gba/gpio/rumble.c
    source/gba/memory/storage/eeprom.c
    source/gba/memory/storage/flash.c
    source/gba/memory/storage/storage.c
    source/gba/memory/dma.c
    source/gba/memory/io.c
    source/gba/memory/memory.c
    source/gba/ppu/background/affine.c
    source/gba/ppu/background/bitmap.c
    source/gba/ppu/background/text.c
    source/gba/ppu/oam.c
    source/gba/ppu/ppu.c
    source/gba/ppu/window.c
    source/gba/channel.c
    source/gba/db.c
    source/gba/debugger.c
    source/gba/gba.c
    source/gba/quicksave.c
    source/gba/scheduler.c
    source/gba/timer.c
)

add_library(gba STATIC ${LIBGBA_SOURCES})
target_link_libraries(gba ${MATH_LIBRARY})

###############################
## Main Executable
###############################

add_executable(hades 
    dreamcast/main.c
    dreamcast/log.c
)

target_link_libraries(hades 
    gba
    SDL2::SDL2
    ${MATH_LIBRARY}
)