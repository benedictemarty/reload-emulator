# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(apple2e
	${CMAKE_CURRENT_SOURCE_DIR}/src/apple2e.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/neo_storage_fatfs.c   # socle: USB drive volume of neo_storage
)

target_compile_options(apple2e PRIVATE -Wall)

target_compile_definitions(apple2e PRIVATE
    PICO_HEAP_SIZE=0x5800   # dvi_init: 3 TMDS buffers + newlib malloc rounding (~21.8 KB measured); link fails if RAM runs short
	# DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
	NEO_FATFS_FILES=1   # One file open: the ProDOS hard disk image
	# DVI_DEFAULT_SERIAL_CONFIG=olimex_rp2040pc_cfg
	# DVI_DEFAULT_SERIAL_CONFIG=pico_zero_cfg
)

target_link_libraries(apple2e
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	tinyusb_host
	fatfs
)

# create map/bin/hex file etc.
pico_add_extra_outputs(apple2e)
neo_multiboot_slot(apple2e "${NEO_SLOT_APPLE2E}")

pico_enable_stdio_usb(apple2e 0)
pico_enable_stdio_uart(apple2e 1)

#=== Bench firmware over SWD (tools/oric_carte.py taper ; screen read in the emulated RAM):
# key queue diag_keyq, frame counter, late scanlines summed by core 1
add_executable(apple2e_diag
	${CMAKE_CURRENT_SOURCE_DIR}/src/apple2e.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/neo_storage_fatfs.c
)
target_compile_options(apple2e_diag PRIVATE -Wall)
target_compile_definitions(apple2e_diag PRIVATE PICO_HEAP_SIZE=0x5800 DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
	NEO_FATFS_FILES=1 APPLE_DIAG=1 PRODOS_HDD_COUNT_READS=diag_hdd_reads)
target_link_libraries(apple2e_diag pico_stdlib pico_multicore pico_util libdvi tinyusb_host fatfs)
pico_add_extra_outputs(apple2e_diag)
pico_enable_stdio_usb(apple2e_diag 0)
pico_enable_stdio_uart(apple2e_diag 1)
