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
