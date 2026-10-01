# CPU of the PC build: W65C02 as on the Neo6502 board (default), or the NMOS 6502
# of the original Oric (undocumented opcodes)
option(ORIC_PC_NMOS "Oric PC build on the NMOS 6502 core (mos6502cpu.h)" OFF)
if (ORIC_PC_NMOS)
    add_compile_definitions(ORIC_NMOS)
endif()

#=== EXECUTABLE: oric

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
    add_executable(oric WIN32 ./src/oric.c)
else()
    add_executable(oric ./src/oric.c)
endif()

target_link_libraries(oric platform)

if (CMAKE_SYSTEM_NAME STREQUAL Emscripten)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    target_link_options(oric PUBLIC --shell-file ../sokol/shell.html)
    target_link_options(oric PUBLIC -sUSE_WEBGL2=1 -sNO_FILESYSTEM=1 -sASSERTIONS=0 -sMALLOC=emmalloc --closure=1)
endif()

if (MSVC)
    target_compile_options(oric PUBLIC /W3)
else()
    target_compile_options(oric PUBLIC -Wall -Wextra -Wsign-compare)
endif()

# explicitly strip dead code
if (CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_SYSTEM_NAME STREQUAL Emscripten)
    target_link_options(oric PRIVATE LINKER:-dead_strip)
endif()

#=== EXECUTABLE: oric_headless (tests, no window)

add_executable(oric_headless ./src/oric_headless.c)
target_include_directories(oric_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (MSVC)
    target_compile_options(oric_headless PUBLIC /W3)
else()
    target_compile_options(oric_headless PUBLIC -Wall -Wextra -Wsign-compare)
endif()

#=== EXECUTABLE: oric_headless_via6522 (trial of the common VIA: the VIA of Neo6502Vic20)

add_executable(oric_headless_via6522 ./src/oric_headless.c ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/chips/via6522.c)
target_include_directories(oric_headless_via6522 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
target_compile_definitions(oric_headless_via6522 PRIVATE ORIC_VIA6522=1)
if (NOT MSVC)
    target_compile_options(oric_headless_via6522 PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME oric_integration_via6522
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/run_integration.sh $<TARGET_FILE:oric_headless_via6522>)
set_tests_properties(oric_integration_via6522 PROPERTIES SKIP_RETURN_CODE 77)

#=== EXECUTABLE: oric_devices_test (unit tests of the disk and tape devices, no ROM needed)

add_executable(oric_devices_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/test_oric_devices.c)
target_include_directories(oric_devices_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
if (NOT MSVC)
    target_compile_options(oric_devices_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME oric_devices COMMAND oric_devices_test)
add_test(NAME oric_integration
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/run_integration.sh $<TARGET_FILE:oric_headless>)
set_tests_properties(oric_integration PROPERTIES SKIP_RETURN_CODE 77)

#=== EXECUTABLE: oric_menu_test (control panel, ORIC.CFG, OSD rendering)

add_executable(oric_menu_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/test_oric_menu.c)
target_include_directories(oric_menu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (NOT MSVC)
    target_compile_options(oric_menu_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME oric_menu COMMAND oric_menu_test)

#=== EXECUTABLE: neo_storage_test (common file volumes: memory and POSIX drivers)

add_executable(neo_storage_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/test_neo_storage.c)
target_include_directories(neo_storage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (NOT MSVC)
    target_compile_options(neo_storage_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME neo_storage COMMAND neo_storage_test)

#=== EXECUTABLE: neo_tnfs_test (TNFS client against a fake server)

add_executable(neo_tnfs_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/test_neo_tnfs.c)
target_include_directories(neo_tnfs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (NOT MSVC)
    target_compile_options(neo_tnfs_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME neo_tnfs COMMAND neo_tnfs_test)

#=== EXECUTABLE: neo_esp_test (TNFS through an ESP8266 AT modem: Neo6502picowifi)

add_executable(neo_esp_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/test_neo_esp.c)
target_include_directories(neo_esp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (NOT MSVC)
    target_compile_options(neo_esp_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME neo_esp COMMAND neo_esp_test)

#=== EXECUTABLE: neo_dgram_test (TNFS through a datagram relay: one length-prefixed frame per datagram)

add_executable(neo_dgram_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/oric/test_neo_dgram.c)
target_include_directories(neo_dgram_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (NOT MSVC)
    target_compile_options(neo_dgram_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME neo_dgram COMMAND neo_dgram_test)
