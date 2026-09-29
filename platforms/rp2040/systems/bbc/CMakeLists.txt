add_executable(bbc
	${CMAKE_CURRENT_SOURCE_DIR}/src/bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)

target_compile_options(bbc PRIVATE -Wall)

target_compile_definitions(bbc PRIVATE
    PICO_HEAP_SIZE=0x5800   # dvi_init: 3 TMDS buffers + newlib malloc rounding (~21.8 KB measured); link fails if RAM runs short
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
)

target_link_libraries(bbc
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	tinyusb_host
	fatfs
	hardware_pio
)
pico_generate_pio_header(bbc ${CMAKE_CURRENT_SOURCE_DIR}/src/bus6502.pio)

# create map/bin/hex file etc.
pico_add_extra_outputs(bbc)
neo_multiboot_slot(bbc "${NEO_SLOT_BBC}")

pico_enable_stdio_usb(bbc 0)
pico_enable_stdio_uart(bbc 1)

#=== bbc_diag: Model B with an on-screen diagnostic overlay (board bring-up)
add_executable(bbc_diag
	${CMAKE_CURRENT_SOURCE_DIR}/src/bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)
target_compile_options(bbc_diag PRIVATE -Wall)
target_compile_definitions(bbc_diag PRIVATE
    PICO_HEAP_SIZE=0x5800   # dvi_init: 3 TMDS buffers + newlib malloc rounding (~21.8 KB measured); link fails if RAM runs short
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
	BBC_DIAG=1
)
target_link_libraries(bbc_diag pico_stdlib pico_multicore pico_util libdvi tinyusb_host fatfs hardware_pio)
pico_generate_pio_header(bbc_diag ${CMAKE_CURRENT_SOURCE_DIR}/src/bus6502.pio)
pico_add_extra_outputs(bbc_diag)
pico_enable_stdio_usb(bbc_diag 0)
pico_enable_stdio_uart(bbc_diag 1)

#=== bbc_master: BBC Master 128 (MOS 3.20 in flash), needs src/roms/bbc_master_roms.h
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/roms/bbc_master_roms.h)
add_executable(bbc_master
	${CMAKE_CURRENT_SOURCE_DIR}/src/bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)
target_compile_options(bbc_master PRIVATE -Wall)
target_compile_definitions(bbc_master PRIVATE
    PICO_HEAP_SIZE=0x5800   # dvi_init: 3 TMDS buffers + newlib malloc rounding (~21.8 KB measured); link fails if RAM runs short
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
	BBC_MASTER=1
	BBC_ROMS_IN_FLASH=1
)
target_link_libraries(bbc_master pico_stdlib pico_multicore pico_util libdvi tinyusb_host fatfs hardware_pio)
pico_generate_pio_header(bbc_master ${CMAKE_CURRENT_SOURCE_DIR}/src/bus6502.pio)
pico_add_extra_outputs(bbc_master)
neo_multiboot_slot(bbc_master "${NEO_SLOT_BBC_MASTER}")
pico_enable_stdio_usb(bbc_master 0)
pico_enable_stdio_uart(bbc_master 1)
endif()
