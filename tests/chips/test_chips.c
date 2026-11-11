// test_chips.c — small chips: AY envelope shapes, mos6522via timers and edges, kbd, beeper, mem
#define CHIPS_IMPL
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "chips/chips_common.h"
#include "chips/ay38910psg.h"
#include "chips/mos6522via.h"
#include "chips/kbd.h"
#include "chips/beeper.h"
#include "chips/mem.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

// The 16 envelope shapes of the AY-3-8910, from the CONT / ATT / ALT / HOLD bits (datasheet)
static void test_ay_shapes(void) {
    for (int s = 0; s < 16; s++) {
        const bool cont = s & 8, att = s & 4, alt = s & 2, hold = s & 1;
        for (int i = 0; i < 16; i++) {
            const int ramp = att ? i : 15 - i;
            CHECK(_ay38910psg_shapes[s][i] == ramp);
        }
        for (int i = 16; i < 32; i++) {
            int expect;
            if (!cont) expect = 0;
            else if (hold) expect = (att != alt) ? 15 : 0;
            else {
                const int j = i - 16;
                const bool up = alt ? !att : att;
                expect = up ? j : 15 - j;
            }
            CHECK(_ay38910psg_shapes[s][i] == expect);
        }
    }
}

// Average T1 period in cycles, in continuous mode, when advanced `step` cycles per call
static double t1_period(int latch, int step) {
    mos6522via_t v;
    mos6522via_init(&v);
    mos6522via_write(&v, 0xB, 0x40);
    mos6522via_write(&v, 0xE, 0xC0);
    mos6522via_write(&v, 0x6, latch & 0xFF);
    mos6522via_write(&v, 0x7, latch >> 8);
    int n = 0;
    const long total = 200000;
    for (long t = 0; t < total; t += step) {
        mos6522via_tick(&v, (uint8_t)step);
        if (v.intr.ifr & 0x40) { n++; mos6522via_read(&v, 0x4); }
    }
    return (double)total / n;
}

static void test_via_timer(void) {
    for (int latch = 98; latch <= 101; latch++) {
        const double p1 = t1_period(latch, 1), p2 = t1_period(latch, 2);
        CHECK(p1 > latch + 2 - 0.1 && p1 < latch + 2 + 0.1);   // N + 2
        CHECK(p2 > latch + 2 - 0.1 && p2 < latch + 2 + 0.1);   // same when advanced 2 by 2
    }
}

// CA1 edge: the flag is raised once; after the CPU cleared it (read of ORA) it stays clear
static void test_via_edge_once(void) {
    mos6522via_t v;
    mos6522via_init(&v);
    mos6522via_write(&v, 0xE, 0x80 | 0x02);   // IER: CA1
    mos6522via_set_ca1(&v, true);
    mos6522via_set_ca1(&v, false);            // falling edge (PCR default: high-to-low)
    for (int i = 0; i < 4; i++) mos6522via_tick(&v, 1);
    CHECK(v.intr.ifr & 0x02);
    mos6522via_read(&v, 0x1);                 // ORA read clears CA1
    for (int i = 0; i < 20; i++) mos6522via_tick(&v, 1);
    CHECK(!(v.intr.ifr & 0x02));
    CHECK(!(v.intr.ifr & 0x80));
}

static void test_kbd(void) {
    kbd_t k;
    kbd_init(&k, 2);
    kbd_register_key(&k, 0, 1, 1, 0);          // key 0 at column 1, line 1
    kbd_register_key(&k, 5, 2, 2, 0);
    kbd_update(&k, 16667);
    kbd_key_down(&k, 0);                       // key code 0 must work
    CHECK(kbd_test_lines(&k, 1 << 1) == (1 << 1));
    kbd_key_up(&k, 0);
    kbd_key_down(&k, 0);                       // pressed again while sticky-released: still held
    for (int i = 0; i < 10; i++) kbd_update(&k, 16667);
    CHECK(kbd_test_lines(&k, 1 << 1) == (1 << 1));
    kbd_key_up(&k, 0);
    for (int i = 0; i < 10; i++) kbd_update(&k, 16667);
    CHECK(kbd_test_lines(&k, 1 << 1) == 0);
}

static void test_beeper(void) {
    beeper_t b;
    beeper_init(&b, &(beeper_desc_t){.tick_hz = 1000000, .sound_hz = 44100, .base_volume = 1.0f});
    CHECK(b.period == (1000000 * BEEPER_FIXEDPOINT_SCALE) / 44100);
    CHECK(b.counter == b.period);
}

static void test_mem(void) {
    static mem_t m;
    static uint8_t rom[0x1000];
    mem_init(&m);
    mem_map_rom(&m, 0, 0xF000, 0x1000, rom);
    rom[0x10] = 0x42;
    CHECK(*mem_readptr(&m, 0xF010) == 0x42);
    mem_wr(&m, 0xF010, 0x99);                  // Write to ROM goes to the junk page
    CHECK(rom[0x10] == 0x42);
}

int main(void) {
    test_ay_shapes();
    test_via_timer();
    test_via_edge_once();
    test_kbd();
    test_beeper();
    test_mem();
    printf("test_chips: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
