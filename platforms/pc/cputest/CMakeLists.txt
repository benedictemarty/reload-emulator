#=== CPU cores: Tom Harte's SingleStepTests (tools/cputest/harte.c) and Klaus
# Dormann's functional tests (tools/cputest/cputest.c); data outside the tree,
# tests skipped when absent (docs/CPU-TESTS.md)

set(CPUTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/cputest)
set(HARTE_DIR "$ENV{HOME}/.cache/65x02" CACHE PATH "SingleStepTests 65x02 (tools/cputest/fetch_harte.sh)")
set(KLAUS_DIR "$ENV{HOME}/.cache/klaus" CACHE PATH "Klaus Dormann's test images (docs/CPU-TESTS.md)")

add_executable(harte_c02 ${CPUTEST_DIR}/harte.c)
target_compile_definitions(harte_c02 PRIVATE USE_65C02)
add_executable(harte_nmos ${CPUTEST_DIR}/harte.c)
add_executable(cputest_c02 ${CPUTEST_DIR}/cputest.c)
target_compile_definitions(cputest_c02 PRIVATE USE_65C02)
add_executable(cputest_nmos ${CPUTEST_DIR}/cputest.c)
foreach(t harte_c02 harte_nmos cputest_c02 cputest_nmos)
    if (NOT MSVC)
        target_compile_options(${t} PRIVATE -O2 -Wall -Wextra)
    endif()
endforeach()

# Known differences (docs/CPU-TESTS.md): WDC $5C (4 cycles in the tests, 8 in the core);
# NMOS JAM (the CPU halts) and the unstable SHA/TAS/SHY/SHX
add_test(NAME cpu_harte_wdc65c02 COMMAND harte_c02 -x 5c ${HARTE_DIR}/wdc65c02/v1)
add_test(NAME cpu_harte_6502
         COMMAND harte_nmos -x 02,12,22,32,42,52,62,72,92,b2,d2,f2,93,9b,9c,9e,9f ${HARTE_DIR}/6502/v1)
set_tests_properties(cpu_harte_wdc65c02 cpu_harte_6502 PROPERTIES SKIP_RETURN_CODE 77)

# Images of the st3fan/ewm repository: success at $3399 (6502) and $24A8 (65C02)
foreach(core c02 nmos)
    add_test(NAME cpu_klaus_6502_${core}
             COMMAND sh -c "[ -f '${KLAUS_DIR}/6502_functional_test.bin' ] || exit 77; exec \"$0\" '${KLAUS_DIR}/6502_functional_test.bin' 3399"
                     $<TARGET_FILE:cputest_${core}>)
    set_tests_properties(cpu_klaus_6502_${core} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
add_test(NAME cpu_klaus_65c02
         COMMAND sh -c "[ -f '${KLAUS_DIR}/65C02_extended_opcodes_test.bin' ] || exit 77; exec \"$0\" '${KLAUS_DIR}/65C02_extended_opcodes_test.bin' 24a8"
                 $<TARGET_FILE:cputest_c02>)
set_tests_properties(cpu_klaus_65c02 PROPERTIES SKIP_RETURN_CODE 77)
