#=== Network bridge on the PC: the twin of the Pico W firmware (platforms/picow/netbridge)

add_executable(neo_netbridge_pc ${CMAKE_CURRENT_SOURCE_DIR}/neo_netbridge_pc.c)
if (NOT MSVC)
    target_compile_options(neo_netbridge_pc PRIVATE -Wall -Wextra)
endif()
