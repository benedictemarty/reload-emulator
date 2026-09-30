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
    PICO_HEAP_SIZE=0x5800   # dvi_init: 3 TMDS buffers + newlib malloc rounding (~21.8 KB measured); link fails if RAM runs short
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

#=== Bench firmwares over SWD (tools/oric_carte.py): key queue diag_keyq, frame and
# emulation time counters; same sources as oric, plus the definitions given
function(oric_diag_variant name)
	add_executable(${name}
		${CMAKE_CURRENT_SOURCE_DIR}/src/oric.c
		${CMAKE_CURRENT_SOURCE_DIR}/src/utils.S
		${CMAKE_CURRENT_SOURCE_DIR}/../../src/hid_app.c
		${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio.c
		${CMAKE_CURRENT_SOURCE_DIR}/../../src/msc_app.c
	)
	target_compile_options(${name} PRIVATE -Wall)
	target_compile_definitions(${name} PRIVATE PICO_HEAP_SIZE=0x5800 DVI_DEFAULT_SERIAL_CONFIG=olimex_neo6502_cfg ORIC_DIAG=1 ${ARGN})
	target_link_libraries(${name} pico_stdlib pico_multicore pico_util libdvi tinyusb_host fatfs hardware_pio)
	pico_generate_pio_header(${name} ${CMAKE_CURRENT_SOURCE_DIR}/../../src/chips/bus6502.pio)
	pico_add_extra_outputs(${name})
	pico_enable_stdio_usb(${name} 0)
	pico_enable_stdio_uart(${name} 1)
endfunction()

oric_diag_variant(oric_diag)                          # Code of oric (bus by wdc65C02cpu.h calls)
oric_diag_variant(oric_diag_cold ORIC_COLD=1)         # Per-cycle code left in flash (before sprint 1)
oric_diag_variant(oric_diag_inline ORIC_BUS_INLINE=1) # SIO bus inlined in oric_tick (wdc65C02bus.h)
oric_diag_variant(oric_diag_pio ORIC_BUS_PIO=1)       # PIO bus (wdc65C02bus.h, bus6502.pio), as the BBC
