#=== Wi-Fi modem simulated on the PC (ESP8266 AT, UDP): the twin of Neo6502picowifi for the tests

add_executable(neo_esp_modem_pc ${CMAKE_CURRENT_SOURCE_DIR}/neo_esp_modem_pc.c)
if (NOT MSVC)
    target_compile_options(neo_esp_modem_pc PRIVATE -Wall -Wextra)
endif()
