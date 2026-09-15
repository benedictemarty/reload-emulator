add_executable(bbc
	${CMAKE_CURRENT_SOURCE_DIR}/src/bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/hid_app_bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/../oric/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)

target_compile_options(bbc PRIVATE -Wall)

target_compile_definitions(bbc PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
)

target_link_libraries(bbc
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	tinyusb_host
	fatfs
)

# create map/bin/hex file etc.
pico_add_extra_outputs(bbc)
neo_multiboot_slot(bbc "${NEO_SLOT_BBC}")

pico_enable_stdio_usb(bbc 0)
pico_enable_stdio_uart(bbc 1)

#=== bbc_master: BBC Master 128 (MOS 3.20 in flash), needs src/roms/bbc_master_roms.h
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/roms/bbc_master_roms.h)
add_executable(bbc_master
	${CMAKE_CURRENT_SOURCE_DIR}/src/bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/hid_app_bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/../oric/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)
target_compile_options(bbc_master PRIVATE -Wall)
target_compile_definitions(bbc_master PRIVATE
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
	BBC_MASTER=1
	BBC_ROMS_IN_FLASH=1
)
target_link_libraries(bbc_master pico_stdlib pico_multicore pico_util libdvi tinyusb_host fatfs)
pico_add_extra_outputs(bbc_master)
pico_enable_stdio_usb(bbc_master 0)
pico_enable_stdio_uart(bbc_master 1)
endif()
