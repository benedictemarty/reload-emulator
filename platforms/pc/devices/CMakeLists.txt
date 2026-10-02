#=== Device tests shared by the systems (tests/devices), no ROM needed

add_executable(hid_media_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/devices/test_hid_media.c)
if (NOT MSVC)
    target_compile_options(hid_media_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME hid_media COMMAND hid_media_test)

add_executable(via6522_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/devices/test_via6522.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/chips/via6522.c)
if (NOT MSVC)
    target_compile_options(via6522_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME via6522 COMMAND via6522_test)

add_executable(osd_panel_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/osd/test_osd_panel.c)
target_include_directories(osd_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
if (NOT MSVC)
    target_compile_options(osd_panel_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME osd_panel COMMAND osd_panel_test)
