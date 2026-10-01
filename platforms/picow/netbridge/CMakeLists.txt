cmake_minimum_required(VERSION 3.13)

# Passerelle réseau Pico W de la Neo6502 (neo_netbridge) : port série USB vers
# Wi-Fi / UDP. SDK et TinyUSB du socle reload ; carte pico_w.
#   cmake -S platforms/picow/netbridge -B build/picow [-DWIFI_SSID=… -DWIFI_PASSWORD=… -DTNFS_SERVER=…]

set(RELOAD_RP2040 ${CMAKE_CURRENT_LIST_DIR}/../../rp2040)
set(PICO_SDK_PATH ${RELOAD_RP2040}/lib/pico-sdk)
set(PICO_TINYUSB_PATH ${RELOAD_RP2040}/lib/tinyusb)
set(PICO_BOARD pico_w)

include(${RELOAD_RP2040}/pico_sdk_import.cmake)
project(neo_netbridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(neo_netbridge main.c)
target_include_directories(neo_netbridge PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../../../src)
target_compile_options(neo_netbridge PRIVATE -Wall)
foreach(v WIFI_SSID WIFI_PASSWORD TNFS_SERVER)
    if (DEFINED ${v})
        target_compile_definitions(neo_netbridge PRIVATE ${v}="${${v}}")
    endif()
endforeach()
target_link_libraries(neo_netbridge pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(neo_netbridge 1)
pico_enable_stdio_uart(neo_netbridge 0)
pico_add_extra_outputs(neo_netbridge)
