cmake_minimum_required(VERSION 3.12)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb)

include(pico_sdk_import.cmake)


project(rp2040 C CXX ASM)

# Neo6502 multi-boot (Neo6502firmware/multiboot, F-80) : link a system for a flash slot.
#   cmake -DNEO_MULTIBOOT_DIR=~/Neo6502firmware/multiboot -DNEO_SLOT_BBC=1 -DNEO_SLOT_ORIC=2 ..
#   (slots: NEO_SLOT_BBC, NEO_SLOT_BBC_MASTER, NEO_SLOT_ORIC, NEO_SLOT_APPLE2, NEO_SLOT_APPLE2E)
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

# PicoDVI (sous-module) : compatibilité pico-sdk 2.x (tcr -> dbg_tcr), sans effet avec le SDK 1.5.1.
# Le correctif est appliqué à la copie de travail du sous-module, de façon idempotente.
execute_process(COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/patches/picodvi-dbg-tcr.sh ${CMAKE_CURRENT_SOURCE_DIR}/lib/PicoDVI
                RESULT_VARIABLE picodvi_patch_rc)
if (NOT picodvi_patch_rc EQUAL 0)
	message(FATAL_ERROR "PicoDVI : échec de patches/picodvi-dbg-tcr.sh")
endif()
# TinyUSB (sous-module) : erreur de séquence de données de l'hôte RP2040 sans panic(),
# reprise Bulk-Only de la clé (patches/tinyusb-data-seq.sh, idempotent)
execute_process(COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/patches/tinyusb-data-seq.sh ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb
                RESULT_VARIABLE tinyusb_patch_rc)
if (NOT tinyusb_patch_rc EQUAL 0)
	message(FATAL_ERROR "TinyUSB : échec de patches/tinyusb-data-seq.sh")
endif()
add_subdirectory(lib/PicoDVI/software/libdvi)
add_subdirectory(lib/fatfs/source)
add_subdirectory(systems)
