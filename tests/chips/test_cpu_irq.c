// test_cpu_irq.c — interrupts of the cycle-stepped cores (mos6502cpu.h; -DUSE_65C02: w65c02cpu.h)
//
// NMI is an edge: every MOS6502CPU_NMI() must trigger one (it used to work once only);
// WAI (65C02 only): CLI; WAI is serviced before the instruction that follows.
#define CHIPS_IMPL
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#ifdef USE_65C02
#include "chips/w65c02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static uint8_t mem[65536];
static MOS6502CPU_T cpu;

static void run(int cycles) {
    for (int i = 0; i < cycles; i++) {
        MOS6502CPU_TICK(&cpu);
        uint16_t a = MOS6502CPU_GET_ADDR(&cpu);
        if (cpu.rw) MOS6502CPU_SET_DATA(&cpu, mem[a]); else mem[a] = MOS6502CPU_GET_DATA(&cpu);
    }
}

static void start(const uint8_t* prog, int n) {
    memset(mem, 0xEA, sizeof(mem));          // NOPs
    memcpy(&mem[0x0200], prog, n);
    mem[0xFFFA] = 0x00; mem[0xFFFB] = 0x03;  // NMI -> $0300
    mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x02;  // RESET -> $0200
    mem[0xFFFE] = 0x00; mem[0xFFFF] = 0x03;  // IRQ -> $0300
    memset(&cpu, 0, sizeof(cpu));
    MOS6502CPU_INIT(&cpu, &(MOS6502CPU_DESC_T){0});
    MOS6502CPU_RESET(&cpu);
    run(20);
}

static void test_nmi_every_time(void) {
    // $0200: JMP $0200 ; $0300: INC $10 ; RTI
    const uint8_t loop[] = {0x4C, 0x00, 0x02};
    start(loop, sizeof(loop));
    const uint8_t handler[] = {0xE6, 0x10, 0x40};
    memcpy(&mem[0x0300], handler, sizeof(handler));
    mem[0x10] = 0;
    for (int i = 1; i <= 3; i++) {
        MOS6502CPU_NMI(&cpu);
        run(40);
        CHECK(mem[0x10] == i);
    }
    run(200);
    CHECK(mem[0x10] == 3);   // No NMI without a new call
}

#ifdef USE_65C02
static void test_wai(void) {
    // CLI ; WAI ; INC $11 ; JMP * ; handler: LDA $11 ; STA $12 ; INC $10 ; RTI
    const uint8_t prog[] = {0x58, 0xCB, 0xE6, 0x11, 0x4C, 0x04, 0x02};
    start(prog, sizeof(prog));
    const uint8_t handler[] = {0xA5, 0x11, 0x85, 0x12, 0xE6, 0x10, 0x40};
    memcpy(&mem[0x0300], handler, sizeof(handler));
    mem[0x10] = mem[0x11] = 0; mem[0x12] = 0xFF;
    run(30);
    CHECK(mem[0x11] == 0);                 // Still waiting
    MOS6502CPU_SET_IRQ(&cpu, true);
    run(12);
    MOS6502CPU_SET_IRQ(&cpu, false);       // The handler has been entered: release the line
    run(60);
    CHECK(mem[0x10] == 1);                 // Handler ran once
    CHECK(mem[0x12] == 0);                 // ... before the instruction after WAI
    CHECK(mem[0x11] == 1);                 // ... which ran afterwards
}

static void test_reset_clears_pending(void) {
    // An NMI pending when RESET comes must not be served after the reset
    const uint8_t loop[] = {0x4C, 0x00, 0x02};
    start(loop, sizeof(loop));
    const uint8_t handler[] = {0xE6, 0x10, 0x40};
    memcpy(&mem[0x0300], handler, sizeof(handler));
    mem[0x10] = 0;
    MOS6502CPU_NMI(&cpu);
    MOS6502CPU_RESET(&cpu);
    run(60);
    CHECK(mem[0x10] == 0);
}
#endif

int main(void) {
    test_nmi_every_time();
#ifdef USE_65C02
    test_wai();
    test_reset_clears_pending();
#endif
    printf("test_cpu_irq: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
