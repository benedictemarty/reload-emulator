# cputest — Klaus Dormann functional tests on the cycle-stepped cores

Builds a 64 KB flat-memory harness around `src/chips/mos6502cpu.h` (NMOS) or
`src/chips/w65c02cpu.h` (`-DUSE_65C02`) and runs a test image loaded at $0000
with the reset vector forced to $0400, until the program loops on itself.

```
gcc -O2 -Isrc -o cputest_nmos tools/cputest/cputest.c
gcc -O2 -DUSE_65C02 -Isrc -o cputest_c02 tools/cputest/cputest.c
./cputest_c02 6502_functional_test.bin 3469          # success PC of the 6502 test
./cputest_c02 65C02_extended_opcodes_test.bin 24f1   # success PC of the 65C02 test (wdc_op=1, rkwl_wdc_op=1)
```

The test images (GPL v3, https://github.com/Klaus2m5/6502_65C02_functional_tests,
`bin_files/`) are not distributed here.
