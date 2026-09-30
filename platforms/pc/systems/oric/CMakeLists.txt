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
