// cputest.c — runs Klaus Dormann's functional tests (64 KB image, start $0400)
// on a cycle-stepped core: -c02 = w65c02cpu.h, default = mos6502cpu.h.
// Ends when PC loops on itself (JMP *): prints the address (success or trap).
#define CHIPS_IMPL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef USE_65C02
#include "chips/w65c02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif

static uint8_t mem[65536];

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: cputest image.bin success_pc_hex [max_cycles]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fread(mem, 1, 65536, f) == 0) { fprintf(stderr, "empty image\n"); return 2; }
    fclose(f);
    uint16_t success = (uint16_t)strtol(argv[2], 0, 16);
    uint64_t max_cycles = argc > 3 ? strtoull(argv[3], 0, 10) : 500000000ULL;
    mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x04;     // Reset vector -> $0400
    MOS6502CPU_T cpu;
    MOS6502CPU_INIT(&cpu, &(MOS6502CPU_DESC_T){0});
    MOS6502CPU_RESET(&cpu);
    uint16_t last_pc = 0xFFFF; int same = 0;
    uint64_t cycles = 0;
    for (; cycles < max_cycles; cycles++) {
        MOS6502CPU_TICK(&cpu);
        uint16_t a = MOS6502CPU_GET_ADDR(&cpu);
        if (cpu.rw) MOS6502CPU_SET_DATA(&cpu, mem[a]); else mem[a] = MOS6502CPU_GET_DATA(&cpu);
        if (cpu.sync) {
            if (cpu.PC == last_pc) { if (++same > 3) break; } else { same = 0; last_pc = cpu.PC; }
        }
    }
    printf("%s : arrêt à PC=%04X après %llu cycles : %s\n", argv[1], last_pc, (unsigned long long)cycles,
           last_pc == success ? "SUCCÈS" : "ÉCHEC (trap)");
    return last_pc == success ? 0 : 1;
}
