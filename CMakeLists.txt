cmake_minimum_required(VERSION 3.15)
project(Hades C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 11)

# Include directories
include_directories(include source)

if(CMAKE_CROSSCOMPILING)
    set(BUILD_DREAMCAST TRUE)
    message(STATUS "Building for Dreamcast")
else()
    set(BUILD_DREAMCAST FALSE)
    message(STATUS "Building for Linux")
endif()

# Platform-specific compiler flags
if(BUILD_DREAMCAST)
    add_compile_options(
        -O3
        -fms-extensions
    )
    add_link_options(-Wl,-Map=output.map)
else()
    add_compile_options(
        -g 
        -pg              
        -fms-extensions
    )
    add_link_options(-Wl,-Map=output.map -pg)
endif()

add_compile_definitions(HADES_VERSION="1.0.0")

# Find dependencies
if(NOT BUILD_DREAMCAST)
    find_package(SDL2 REQUIRED)
    find_library(MATH_LIBRARY m)
    find_package(Threads REQUIRED)
endif()

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

if(BUILD_DREAMCAST)
    target_link_libraries(gba m)
else()
    target_link_libraries(gba ${MATH_LIBRARY})
endif()

###############################
## Main Executable
###############################

if(BUILD_DREAMCAST)
    add_executable(hades 
        dreamcast/main.c
        dreamcast/log.c
    )
    
    target_link_libraries(hades 
        gba
        m
        pthread
    )
else()
    add_executable(hades 
        linux/main.c
        linux/log.c
    )
    
    target_link_libraries(hades 
        gba
        SDL2::SDL2
        ${MATH_LIBRARY}
        Threads::Threads
    )
endif()



if(BUILD_DREAMCAST)
    add_custom_command(TARGET hades POST_BUILD
        COMMAND mkdcdisc -e $<TARGET_FILE:hades> -d ../assets -o hades.cdi -N
        COMMENT "Creating Dreamcast CDI disc image..."
        VERBATIM
    )
endif()
