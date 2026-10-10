#=== EXECUTABLE: bbc

if(CMAKE_SYSTEM_NAME STREQUAL Windows)
    add_executable(bbc WIN32 ./src/bbc.c)
else()
    add_executable(bbc ./src/bbc.c)
endif()

target_link_libraries(bbc platform)

if (MSVC)
    target_compile_options(bbc PUBLIC /W3)
else()
    target_compile_options(bbc PUBLIC -Wall -Wextra -Wsign-compare)
endif()

#=== EXECUTABLE: bbc_headless (tests, no window)

add_executable(bbc_headless ./src/bbc_headless.c)
target_include_directories(bbc_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)

if (MSVC)
    target_compile_options(bbc_headless PUBLIC /W3)
else()
    target_compile_options(bbc_headless PUBLIC -Wall -Wextra -Wsign-compare)
endif()

#=== EXECUTABLE: bbc_headless_lut (tests: byte -> pixels tables built as on the RP2040)

add_executable(bbc_headless_lut ./src/bbc_headless.c)
target_include_directories(bbc_headless_lut PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
target_compile_definitions(bbc_headless_lut PRIVATE BBC_LUT_PROGRESSIVE=16 BBC_LUT_SLOTS=8)
if (NOT MSVC)
    target_compile_options(bbc_headless_lut PUBLIC -Wall -Wextra -Wsign-compare)
endif()

#=== EXECUTABLE: bbc_menu_test (control panel of the board, src/osd/bbc_menu.h)

add_executable(bbc_menu_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/bbc/test_bbc_menu.c)
target_include_directories(bbc_menu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
if (NOT MSVC)
    target_compile_options(bbc_menu_test PUBLIC -Wall -Wextra -Wsign-compare)
endif()
add_test(NAME bbc_menu COMMAND bbc_menu_test)

#=== Integration tests (tests/bbc/test_bbc_pc.py, Python 3 + Pillow): boot, BASIC,
# screens compared to references, sound, joystick, disc write, Master 128, Tube,
# bbc_tick4 = bbc_tick, progressive tables = PC rendering (docs/BBC.md)
add_test(NAME bbc_integration COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/bbc/test_bbc_pc.py)
set_tests_properties(bbc_integration PROPERTIES SKIP_RETURN_CODE 77
    ENVIRONMENT "BBC_HEADLESS=$<TARGET_FILE:bbc_headless>;BBC_HEADLESS_LUT=$<TARGET_FILE:bbc_headless_lut>")
