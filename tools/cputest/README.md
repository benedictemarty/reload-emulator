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
`bin_files/`) are not distributed here. The images of the st3fan/ewm repository
(`roms/`) succeed at $3399 and $24A8 instead (CTest `cpu_klaus_*`, `~/.cache/klaus`).

# harte — Tom Harte's SingleStepTests

`harte.c` runs https://github.com/SingleStepTests/65x02 (one instruction per test,
bus operation of every cycle) on the same cores: `-DUSE_65C02` for `wdc65c02/v1`,
default for `6502/v1`. `fetch_harte.sh` downloads them into `~/.cache/65x02`.

```
./harte_c02 -x 5c ~/.cache/65x02/wdc65c02/v1          # all opcodes, $5C known difference
./harte_nmos -v ~/.cache/65x02/6502/v1 69 e9          # every failure of two opcodes
```

Functional failures (registers, RAM, cycle count, writes) fail the run; read-only
bus differences (dummy reads) are reported. Results and known differences:
docs/CPU-TESTS.md.
