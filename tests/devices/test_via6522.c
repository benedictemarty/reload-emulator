// test_via6522.c — VIA unique (src/chips/via6522.c) : avance par lots et lecture sans effet
//
// via_advance(n) doit donner exactement le même état que n appels de via_tick (IRQ, PB7,
// CB2, registres), quelle que soit la suite d'accès ; via_peek ne doit rien modifier.
// Deux VIA reçoivent la même suite aléatoire d'opérations (écritures, lectures, fronts
// CA1/CB1) ; l'une avance cycle par cycle, l'autre par pas de 4 ou de longueur aléatoire.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chips/via6522.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint8_t port_in(void* u) {
    (void)u;
    return 0xA5;
}

// Same observable state: IRQ line, PB7, CB2, CA2 and the 16 registers as via_read would give them
static bool same(const via6522_t* a, const via6522_t* b) {
    if (a->irq_line != b->irq_line) return false;
    if (via_get_pb7((via6522_t*)a) != via_get_pb7((via6522_t*)b)) return false;
    if (via_get_cb2((via6522_t*)a) != via_get_cb2((via6522_t*)b)) return false;
    if (via_get_ca2((via6522_t*)a) != via_get_ca2((via6522_t*)b)) return false;
    for (uint8_t r = 0; r < 16; r++) {
        if (via_peek(a, r) != via_peek(b, r)) return false;
    }
    return true;
}

static void op(via6522_t* v, uint32_t kind, uint32_t arg) {
    switch (kind % 9) {
        case 0: via_write(v, VIA_T1CL, (uint8_t)arg); via_write(v, VIA_T1CH, (uint8_t)(arg >> 8) & 0x03); break;
        case 1: via_write(v, VIA_T2CL, (uint8_t)arg); via_write(v, VIA_T2CH, (uint8_t)(arg >> 8) & 0x01); break;
        case 2: via_write(v, VIA_ACR, (uint8_t)(arg & 0xE3)); break;   // T1/T2 modes, latching (no shift register)
        case 3: via_write(v, VIA_IER, (uint8_t)arg); break;
        case 4: via_write(v, VIA_IFR, (uint8_t)arg); break;
        case 5: via_write(v, VIA_PCR, (uint8_t)arg); break;
        case 6: (void)via_read(v, (uint8_t)(arg & 15)); break;   // Reads clear flags
        case 7: via_set_ca1(v, arg & 1); break;
        case 8: via_set_cb1(v, arg & 1); break;
    }
}

static void test_advance(bool free_counters) {
    static via6522_t a, b;
    via_init(&a);
    via_init(&b);
    via_reset(&a);
    via_reset(&b);
    if (free_counters) {
        via_set_free_counters(&a, true);
        via_set_free_counters(&b, true);
    }
    via_set_port_callbacks(&a, port_in, 0, port_in, 0, 0);
    via_set_port_callbacks(&b, port_in, 0, port_in, 0, 0);
    via_write(&a, VIA_DDRB, 0x80);
    via_write(&b, VIA_DDRB, 0x80);
    int bad = 0, steps = 0, irqs = 0;
    for (int n = 0; n < 200000; n++) {
        if ((rnd() & 7) == 0) {
            const uint32_t k = rnd(), g = rnd();
            op(&a, k, g);
            op(&b, k, g);
        }
        if ((rnd() & 4095) == 0) {   // Reset now and then: latches and counters kept
            via_reset(&a);
            via_reset(&b);
        }
        // Step: 4 cycles mostly (as the Oric and the Telestrat), sometimes 1 to 40
        const int len = (rnd() & 3) ? 4 : 1 + (int)(rnd() % 40);
        for (int i = 0; i < len; i++) via_tick(&a);
        via_advance(&b, len);
        steps++;
        irqs += a.irq_line;
        if (!same(&a, &b)) bad++;
    }
    CHECK(bad == 0);
    CHECK(irqs > 1000);   // The random sequence really raised interrupts
    printf("  compteurs libres %d : %d pas, %d avec IRQ, %d écarts\n", free_counters, steps, irqs, bad);
}

static void test_peek(void) {
    static via6522_t v, before;
    via_init(&v);
    via_write(&v, VIA_IER, 0x80 | VIA_INT_T1 | VIA_INT_T2);
    via_write(&v, VIA_T1CL, 10);
    via_write(&v, VIA_T1CH, 0);
    via_write(&v, VIA_T2CL, 5);
    via_write(&v, VIA_T2CH, 0);
    for (int i = 0; i < 30; i++) via_tick(&v);
    CHECK((v.ifr & (VIA_INT_T1 | VIA_INT_T2)) == (VIA_INT_T1 | VIA_INT_T2) && v.irq_line);
    before = v;
    for (uint8_t r = 0; r < 16; r++) (void)via_peek(&v, r);
    CHECK(!memcmp(&before, &v, sizeof(v)));   // Nothing changed
    const uint8_t t1 = via_peek(&v, VIA_T1CL);
    CHECK(t1 == via_read(&v, VIA_T1CL));      // Same value as the read...
    CHECK(!(v.ifr & VIA_INT_T1));             // ...which clears the flag
}

// Neo6502TeleStrat (STRATSED, LDA #$7F : STA IER) : disabling the active source releases
// the IRQ at once; the T1 flag stays set, IFR bit 7 (any enabled flag) goes to 0
static void test_ier_release(void) {
    static via6522_t v;
    via_init(&v);
    via_write(&v, VIA_IER, 0x80 | VIA_INT_T1);
    via_write(&v, VIA_T1CL, 10);
    via_write(&v, VIA_T1CH, 0);
    for (int i = 0; i < 20; i++) via_tick(&v);
    CHECK(v.irq_line);
    via_write(&v, VIA_IER, VIA_INT_T1);   // Bit 7 = 0: disable T1
    CHECK(!v.irq_line);
    CHECK(!(via_peek(&v, VIA_IFR) & 0x80));
    CHECK(via_peek(&v, VIA_IFR) & VIA_INT_T1);
}

// Rockwell datasheet: reset keeps T1/T2 latches and counters (and SR); counters stop
// unless via_set_free_counters
static void test_reset_keeps(void) {
    static via6522_t v;
    via_init(&v);
    via_reset(&v);
    CHECK(via_peek(&v, VIA_T1CL) == 0xFF && via_peek(&v, VIA_T1CH) == 0xFF);
    via_write(&v, VIA_T1LL, 0x34);
    via_write(&v, VIA_T1LH, 0x12);
    via_write(&v, VIA_T2CL, 0x78);
    via_write(&v, VIA_T2CH, 0x56);
    via_write(&v, VIA_SR, 0x9A);
    for (int i = 0; i < 10; i++) via_tick(&v);
    const uint8_t t2l = via_peek(&v, VIA_T2CL);
    via_reset(&v);
    CHECK(via_peek(&v, VIA_T1LL) == 0x34 && via_peek(&v, VIA_T1LH) == 0x12);
    CHECK(via_peek(&v, VIA_T2CL) == t2l && via_peek(&v, VIA_T2CH) == 0x56);
    CHECK(via_peek(&v, VIA_SR) == 0x9A);
    for (int i = 0; i < 10; i++) via_tick(&v);
    CHECK(via_peek(&v, VIA_T2CL) == t2l);      // Stopped by default
    via_set_free_counters(&v, true);
    via_reset(&v);
    for (int i = 0; i < 10; i++) via_tick(&v);
    CHECK(via_peek(&v, VIA_T2CL) == (uint8_t)(t2l - 10));   // Counting, no IRQ
    CHECK(!v.irq_line && !(v.ifr & (VIA_INT_T1 | VIA_INT_T2)));
}

int main(void) {
    test_ier_release();
    test_reset_keeps();
    test_advance(false);
    test_advance(true);
    test_peek();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
