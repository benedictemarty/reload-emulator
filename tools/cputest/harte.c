// harte.c — runs Tom Harte's SingleStepTests (https://github.com/SingleStepTests/65x02)
// on a cycle-stepped core: -DUSE_65C02 = w65c02cpu.h (set wdc65c02/v1),
// default = mos6502cpu.h (set 6502/v1).
//
// Each test is one instruction: initial registers and RAM, the expected bus
// operation of every cycle ([address, value, read|write]) and the final state.
// The core starts on the opcode fetch (SYNC, address = PC); the instruction ends
// when the core issues the next opcode fetch (SYNC again), as in cputest.c.
//
//   harte [-b] [-v] [-x OP,OP…] DIR [OPCODE_HEX…]   files DIR/xx.json (all present if no
//   opcode given); -b: first bus-only difference of each opcode, -v: every functional
//   failure, -x: known differences, run and reported but not counted as failures
//   exit: 0 no functional failure, 1 functional failures, 77 DIR missing (skipped by CTest)
//
// Two levels per test:
// - functional: final registers and RAM, number of cycles, written bytes (address,
//   value, order): what a program and the memory-mapped devices see;
// - bus: in addition, the address and value of every read cycle, dummy reads
//   included (they matter only if they hit an I/O register).
// P is compared without bits 4 (B) and 5, which are not stored in the core.
#define CHIPS_IMPL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef USE_65C02
#include "chips/w65c02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif

#define MAX_RAM    64
#define MAX_CYCLES 16
#define P_MASK     0xCF

typedef struct {
    uint16_t pc;
    uint8_t s, a, x, y, p;
    int num_ram;
    uint16_t ram_addr[MAX_RAM];
    uint8_t ram_val[MAX_RAM];
} state_t;

typedef struct {
    char name[32];
    state_t initial, final;
    int num_cycles;
    uint16_t cyc_addr[MAX_CYCLES];
    uint8_t cyc_val[MAX_CYCLES];
    bool cyc_read[MAX_CYCLES];
} test_t;

/*-- Minimal JSON reader for this format ------------------------------------*/

static const char* p;

static void ws(void) { while (*p && isspace((unsigned char)*p)) p++; }
static bool eat(char ch) { ws(); if (*p == ch) { p++; return true; } return false; }
static void need(char ch) {
    if (!eat(ch)) { fprintf(stderr, "JSON : '%c' attendu près de « %.20s »\n", ch, p); exit(2); }
}
static long num(void) { ws(); char* e; long v = strtol(p, &e, 10); if (e == p) need('0'); p = e; return v; }
static void str(char* out, size_t n) {
    need('"');
    size_t i = 0;
    while (*p && *p != '"') { if (out && i + 1 < n) out[i++] = *p; p++; }
    if (out) out[i] = 0;
    need('"');
}

static void parse_state(state_t* st) {
    char key[16];
    need('{');
    do {
        str(key, sizeof(key));
        need(':');
        if (!strcmp(key, "ram")) {
            st->num_ram = 0;
            need('[');
            if (!eat(']')) {
                do {
                    need('[');
                    long a = num(); need(','); long v = num();
                    need(']');
                    if (st->num_ram < MAX_RAM) {
                        st->ram_addr[st->num_ram] = (uint16_t)a;
                        st->ram_val[st->num_ram] = (uint8_t)v;
                        st->num_ram++;
                    }
                } while (eat(','));
                need(']');
            }
        } else {
            long v = num();
            if (!strcmp(key, "pc")) st->pc = (uint16_t)v;
            else if (!strcmp(key, "s")) st->s = (uint8_t)v;
            else if (!strcmp(key, "a")) st->a = (uint8_t)v;
            else if (!strcmp(key, "x")) st->x = (uint8_t)v;
            else if (!strcmp(key, "y")) st->y = (uint8_t)v;
            else if (!strcmp(key, "p")) st->p = (uint8_t)v;
        }
    } while (eat(','));
    need('}');
}

static void parse_test(test_t* t) {
    char key[16];
    need('{');
    do {
        str(key, sizeof(key));
        need(':');
        if (!strcmp(key, "name")) str(t->name, sizeof(t->name));
        else if (!strcmp(key, "initial")) parse_state(&t->initial);
        else if (!strcmp(key, "final")) parse_state(&t->final);
        else if (!strcmp(key, "cycles")) {
            t->num_cycles = 0;
            need('[');
            if (!eat(']')) {
                do {
                    char type[8];
                    need('[');
                    long a = num(); need(','); long v = num(); need(','); str(type, sizeof(type));
                    need(']');
                    if (t->num_cycles < MAX_CYCLES) {
                        t->cyc_addr[t->num_cycles] = (uint16_t)a;
                        t->cyc_val[t->num_cycles] = (uint8_t)v;
                        t->cyc_read[t->num_cycles] = !strcmp(type, "read");
                        t->num_cycles++;
                    }
                } while (eat(','));
                need(']');
            }
        } else {
            fprintf(stderr, "JSON : clé inconnue %s\n", key);
            exit(2);
        }
    } while (eat(','));
    need('}');
}

/*-- Running one test ------------------------------------------------------*/

static uint8_t mem[65536];

static uint8_t get_p(const MOS6502CPU_T* c) {
    return (uint8_t)((c->nf ? 0x80 : 0) | (c->vf ? 0x40 : 0) | (c->df ? 0x08 : 0) | (c->iflag ? 0x04 : 0) |
                     (c->zf ? 0x02 : 0) | (c->cf ? 0x01 : 0));
}

static void setup(MOS6502CPU_T* c, const state_t* st) {
    MOS6502CPU_INIT(c, &(MOS6502CPU_DESC_T){0});
    c->res = false;
    c->PC = st->pc;
    c->A = st->a; c->X = st->x; c->Y = st->y; c->S = st->s;
    c->nf = st->p & 0x80; c->vf = st->p & 0x40; c->df = st->p & 0x08;
    c->iflag = st->p & 0x04; c->zf = st->p & 0x02; c->cf = st->p & 0x01;
    // Opcode fetch cycle at PC, as after the previous instruction
    c->sync = true;
    c->rw = true;
    c->addr = st->pc;
#ifdef USE_65C02
    c->step = 0;
#else
    // Invariant of mos6502cpu.h (_set_flags): B always set in the register, bit 5 clear
    c->bf = true;
    c->xf = false;
#endif
}

static char report[256];

enum { PASS, BUS_ONLY, FUNCTIONAL };

// Runs the test: PASS, BUS_ONLY (only read cycles differ) or FUNCTIONAL (report[] says why)
static int run(const test_t* t) {
    for (int i = 0; i < t->initial.num_ram; i++) mem[t->initial.ram_addr[i]] = t->initial.ram_val[i];
    MOS6502CPU_T cpu;
    setup(&cpu, &t->initial);
    int n = 0, nw = 0;
    bool ok = true, bus_ok = true;
    uint16_t w_addr[MAX_CYCLES];
    uint8_t w_val[MAX_CYCLES];
    report[0] = 0;
    do {
        uint16_t a = MOS6502CPU_GET_ADDR(&cpu);
        bool rd = cpu.rw;
        uint8_t v;
        if (rd) { v = mem[a]; MOS6502CPU_SET_DATA(&cpu, v); } else { v = MOS6502CPU_GET_DATA(&cpu); mem[a] = v; }
        if (!rd && nw < MAX_CYCLES) { w_addr[nw] = a; w_val[nw] = v; nw++; }
        if (bus_ok && n < t->num_cycles && (a != t->cyc_addr[n] || rd != t->cyc_read[n] || v != t->cyc_val[n])) {
            snprintf(report, sizeof(report), "cycle %d : %04X %02X %s, attendu %04X %02X %s", n + 1, a, v,
                     rd ? "read" : "write", t->cyc_addr[n], t->cyc_val[n], t->cyc_read[n] ? "read" : "write");
            bus_ok = false;
        }
        n++;
        MOS6502CPU_TICK(&cpu);
    } while (!cpu.sync && n < 64);
    char bus_report[256];
    memcpy(bus_report, report, sizeof(report));
    if (n != t->num_cycles) {
        snprintf(report, sizeof(report), "%d cycles, attendu %d", n, t->num_cycles);
        ok = false;
    }
    int ew = 0;
    for (int i = 0; ok && i < t->num_cycles; i++) {
        if (t->cyc_read[i]) continue;
        if (ew >= nw || w_addr[ew] != t->cyc_addr[i] || w_val[ew] != t->cyc_val[i]) {
            snprintf(report, sizeof(report), "écriture %d : attendu %04X %02X", ew + 1, t->cyc_addr[i], t->cyc_val[i]);
            ok = false;
        }
        ew++;
    }
    if (ok && ew != nw) {
        snprintf(report, sizeof(report), "%d écritures, attendu %d", nw, ew);
        ok = false;
    }
    const state_t* f = &t->final;
    if (ok && (cpu.PC != f->pc || cpu.A != f->a || cpu.X != f->x || cpu.Y != f->y || cpu.S != f->s ||
               (get_p(&cpu) & P_MASK) != (f->p & P_MASK))) {
        snprintf(report, sizeof(report),
                 "registres PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X, attendu PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X",
                 cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.S, get_p(&cpu) & P_MASK, f->pc, f->a, f->x, f->y, f->s, f->p & P_MASK);
        ok = false;
    }
    for (int i = 0; ok && i < f->num_ram; i++) {
        if (mem[f->ram_addr[i]] != f->ram_val[i]) {
            snprintf(report, sizeof(report), "RAM %04X=%02X, attendu %02X", f->ram_addr[i], mem[f->ram_addr[i]],
                     f->ram_val[i]);
            ok = false;
        }
    }
    // Clean memory for the next test (only the addresses a test may touch are listed)
    for (int i = 0; i < t->initial.num_ram; i++) mem[t->initial.ram_addr[i]] = 0;
    for (int i = 0; i < f->num_ram; i++) mem[f->ram_addr[i]] = 0;
    for (int i = 0; i < t->num_cycles; i++) mem[t->cyc_addr[i]] = 0;
    if (!ok) return FUNCTIONAL;
    if (!bus_ok) { memcpy(report, bus_report, sizeof(report)); return BUS_ONLY; }
    return PASS;
}

/*-- One file per opcode ----------------------------------------------------*/

static bool verbose_bus;   // -b: first bus-only difference of each opcode
static bool verbose_all;   // -v: every functional failure
static bool excluded[256]; // -x: known differences (docs/CPU-TESTS.md)

static char* slurp(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0) { fclose(fp); return NULL; }   // Empty file: opcode not tested (WAI, STP)
    fseek(fp, 0, SEEK_SET);
    char* buf = malloc((size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(buf); return NULL; }
    buf[size] = 0;
    fclose(fp);
    return buf;
}

// Returns the number of functional failures, -1 if the file is absent; adds the
// bus-only differences to *bus
static int run_file(const char* dir, int op, int* total, int* bus) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%02x.json", dir, op);
    char* buf = slurp(path);
    if (!buf) return -1;
    p = buf;
    int failed = 0, bus_only = 0, count = 0;
    test_t t;
    need('[');
    if (!eat(']')) {
        do {
            memset(&t, 0, sizeof(t));
            parse_test(&t);
            count++;
            int r = run(&t);
            if (r == FUNCTIONAL) {
                if (failed < 3 || verbose_all) printf("  %02x « %s » : %s\n", op, t.name, report);
                failed++;
            } else if (r == BUS_ONLY) {
                if (bus_only < 1 && verbose_bus) printf("  %02x « %s » (bus) : %s\n", op, t.name, report);
                bus_only++;
            }
        } while (eat(','));
        need(']');
    }
    free(buf);
    *total += count;
    *bus += bus_only;
    if (failed || bus_only || verbose_bus)
        printf("%02x : %5d tests, %5d échecs fonctionnels, %5d écarts de bus seuls\n", op, count, failed, bus_only);
    return failed;
}

int main(int argc, char** argv) {
    while (argc > 1 && argv[1][0] == '-') {
        if (!strcmp(argv[1], "-b")) verbose_bus = true;
        else if (!strcmp(argv[1], "-v")) verbose_all = true;
        else if (!strcmp(argv[1], "-x") && argc > 2) {
            for (char* q = argv[2]; *q;) {
                excluded[strtol(q, &q, 16) & 0xFF] = true;
                if (*q == ',') q++; else if (*q) break;
            }
            argv++; argc--;
        }
        argv++; argc--;
    }
    if (argc < 2) { fprintf(stderr, "usage: harte [-b] [-v] [-x OP,OP…] DIR [OPCODE_HEX…]\n"); return 2; }
    struct stat sb;
    if (stat(argv[1], &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        printf("%s absent : tests ignorés (tools/cputest/fetch_harte.sh)\n", argv[1]);
        return 77;
    }
    int total = 0, failed = 0, bus = 0, files = 0, failed_ops = 0, known = 0;
    for (int op = 0; op < 256; op++) {
        if (argc > 2) {
            bool wanted = false;
            for (int i = 2; i < argc; i++) wanted |= (strtol(argv[i], 0, 16) == op);
            if (!wanted) continue;
        }
        int f = run_file(argv[1], op, &total, &bus);
        if (f < 0) continue;
        files++;
        if (excluded[op]) { if (f) printf("   (%02x : écart connu, non compté)\n", op); known += f; continue; }
        failed += f;
        failed_ops += (f > 0);
    }
    printf("%s : %d fichiers, %d tests, %d échecs fonctionnels (%d opcodes), %d écarts connus, %d écarts de bus seuls\n",
#ifdef USE_65C02
           "w65c02cpu.h",
#else
           "mos6502cpu.h",
#endif
           files, total, failed, failed_ops, known, bus);
    if (files == 0) return 77;
    return failed ? 1 : 0;
}
