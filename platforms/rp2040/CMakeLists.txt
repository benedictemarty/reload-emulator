cmake_minimum_required(VERSION 3.12)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb)

include(pico_sdk_import.cmake)

# Local fixes to the TinyUSB submodule (applied once, left in its working tree):
# pause interrupt endpoint polling during EPX transfers (USB drive + keyboard)
foreach(patch ${CMAKE_CURRENT_SOURCE_DIR}/patches/tinyusb-epx-pause-int-eps.patch)
    execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb apply --reverse --check ${patch}
                    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
        execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb apply ${patch} RESULT_VARIABLE applied)
        if(NOT applied EQUAL 0)
            message(WARNING "TinyUSB patch not applied: ${patch}")
        endif()
    endif()
endforeach()

project(rp2040 C CXX ASM)

# Neo6502 multi-boot (Neo6502firmware/multiboot, F-80) : link a system for a flash slot.
#   cmake -DNEO_MULTIBOOT_DIR=~/Neo6502firmware/multiboot -DNEO_SLOT_BBC=1 -DNEO_SLOT_ORIC=2 ..
set(NEO_MULTIBOOT_DIR "" CACHE PATH "Neo6502firmware/multiboot directory (memmap_slot_N.ld)")
function(neo_multiboot_slot target slot)
    if (NOT "${slot}" STREQUAL "")
        if ("${NEO_MULTIBOOT_DIR}" STREQUAL "")
            message(FATAL_ERROR "NEO_MULTIBOOT_DIR must point to Neo6502firmware/multiboot")
        endif()
        pico_set_linker_script(${target} ${NEO_MULTIBOOT_DIR}/memmap_slot_${slot}.ld)
        target_compile_definitions(${target} PRIVATE NEO_MULTIBOOT_SLOT=${slot})
        message(STATUS "${target} linked for Neo6502 multi-boot slot ${slot}")
    endif()
endfunction()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(PICO_BOARD olimex_neo6502)
# set(PICO_BOARD olimex_rp2040pc)
# set(PICO_BOARD pico_zero)

pico_sdk_init()

add_compile_options(-Wall)

include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/lib/PicoDVI/software/include
	${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/source
	${CMAKE_CURRENT_SOURCE_DIR}/../../src
	${CMAKE_CURRENT_SOURCE_DIR}/src
	)

add_subdirectory(lib/PicoDVI/software/libdvi)
add_subdirectory(lib/fatfs/source)
add_subdirectory(systems)
