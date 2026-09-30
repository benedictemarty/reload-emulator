# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(oric
	${CMAKE_CURRENT_SOURCE_DIR}/src/oric.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)

target_compile_options(oric PRIVATE -Wall)

target_compile_definitions(oric PRIVATE
	# DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
	DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg
	# DVI_DEFAULT_SERIAL_CONFIG=olimex_rp2040pc_cfg
	# DVI_DEFAULT_SERIAL_CONFIG=pico_zero_cfg
)

target_link_libraries(oric
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
	tinyusb_host
	fatfs
)

# create map/bin/hex file etc.
pico_add_extra_outputs(oric)
neo_multiboot_slot(oric "${NEO_SLOT_ORIC}")

pico_enable_stdio_usb(oric 0)
pico_enable_stdio_uart(oric 1)

#=== oric_diag: bench tests over SWD (key queue diag_keyq, frame counters)
add_executable(oric_diag
	${CMAKE_CURRENT_SOURCE_DIR}/src/oric.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
)
target_compile_options(oric_diag PRIVATE -Wall)
target_compile_definitions(oric_diag PRIVATE DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg ORIC_DIAG=1)
target_link_libraries(oric_diag pico_stdlib pico_multicore pico_util libdvi tinyusb_host fatfs)
pico_add_extra_outputs(oric_diag)
pico_enable_stdio_usb(oric_diag 0)
pico_enable_stdio_uart(oric_diag 1)
