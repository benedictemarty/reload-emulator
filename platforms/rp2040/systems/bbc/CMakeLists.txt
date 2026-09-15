add_executable(bbc
	${CMAKE_CURRENT_SOURCE_DIR}/src/bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/hid_app_bbc.c
	${CMAKE_CURRENT_SOURCE_DIR}/../oric/src/utils.S
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
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
)

# create map/bin/hex file etc.
pico_add_extra_outputs(bbc)

pico_enable_stdio_usb(bbc 0)
pico_enable_stdio_uart(bbc 1)
