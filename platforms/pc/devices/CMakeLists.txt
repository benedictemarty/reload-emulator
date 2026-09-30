#=== Device tests shared by the systems (tests/devices), no ROM needed

add_executable(hid_media_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/devices/test_hid_media.c)
if (NOT MSVC)
    target_compile_options(hid_media_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME hid_media COMMAND hid_media_test)
