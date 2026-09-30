#pragma once
// wdc65C02bus.h
//
// Bus cycle of the real W65C02 on the Olimex Neo6502, inlined in the system's
// tick function (include after chips/wdc65C02cpu.h, which sets up the pins):
// - default: direct SIO register accesses (same sequence and delays as
//   wdc65C02cpu.h, without the calls);
// - WDC65C02_BUS_PIO: cycles sequenced by a PIO state machine (bus6502.pio, next
//   to this file), the transceiver delays overlap with the emulation work; call
//   bus_pio_start() once after wdc65C02cpu_init(). PIO block and state machine:
//   WDC65C02_BUS_PIO_BLOCK (pio1) and WDC65C02_BUS_PIO_SM (0); PicoDVI uses pio0.
// Replaces MOS6502CPU_TICK / GET_ADDR / GET_DATA / SET_DATA.
//
// First written for the BBC Micro (platforms/rp2040/systems/bbc/src/bbc.c).

#ifdef OLIMEX_NEO6502
#include "hardware/structs/sio.h"
#define BUS_NOP6() __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\n")

static inline __attribute__((always_inline)) void bus_tick(wdc6502cpu_t *c) {
    sio_hw->gpio_clr = 1u << _CLOCK_PIN;
    sio_hw->gpio_set = 1u << _OE3_PIN;   // End of the data of a read cycle, after PHI2 fell (see bus_set_data)
    sio_hw->gpio_oe_clr = _GPIO_MASK;
    sio_hw->gpio_clr = 1u << _OE1_PIN;
    BUS_NOP6();
    uint32_t lo = sio_hw->gpio_in & 0xFF;
    sio_hw->gpio_set = 1u << _OE1_PIN;
    sio_hw->gpio_clr = 1u << _OE2_PIN;
    BUS_NOP6();
    uint32_t hi = sio_hw->gpio_in & 0xFF;
    sio_hw->gpio_set = 1u << _OE2_PIN;
    c->addr = (uint16_t)(lo | (hi << 8));
    c->rw = (sio_hw->gpio_in >> _RW_PIN) & 1;
    sio_hw->gpio_set = 1u << _CLOCK_PIN;
}

static inline __attribute__((always_inline)) uint8_t bus_get_data(void) {
    sio_hw->gpio_oe_clr = _GPIO_MASK;
    sio_hw->gpio_clr = 1u << _OE3_PIN;
    BUS_NOP6();
    uint8_t data = (uint8_t)(sio_hw->gpio_in & 0xFF);
    sio_hw->gpio_set = 1u << _OE3_PIN;
    return data;
}

// Read cycle: the data stays driven (OE3 low) until PHI2 falls at the next
// bus_tick, where the 65C02 latches it. A brief OE3 pulse left the byte on the
// bus capacitance only, and it leaked away when the emulation paused with PHI2
// high (end of a frame: milliseconds). OE3 goes high right after PHI2 falls,
// before the 65C02 changes R/W for the next cycle (tADS ~30 ns).
static inline __attribute__((always_inline)) void bus_set_data(uint8_t data) {
    sio_hw->gpio_oe_set = _GPIO_MASK;
    sio_hw->gpio_togl = (sio_hw->gpio_out ^ data) & _GPIO_MASK;
    sio_hw->gpio_clr = 1u << _OE3_PIN;
}

#ifdef WDC65C02_BUS_PIO
// The core only exchanges FIFO words with the state machine. Exactly one command
// per 65C02 cycle: an access nobody answered is closed at the next one.
// BBC Micro, BASIC at rest: same speed as SIO; Knight Lore (many write cycles):
// 17.97 ms per 20 ms frame instead of 20.69.
#include "hardware/pio.h"
#include "bus6502.pio.h"
#ifndef WDC65C02_BUS_PIO_BLOCK
#define WDC65C02_BUS_PIO_BLOCK pio1
#endif
#ifndef WDC65C02_BUS_PIO_SM
#define WDC65C02_BUS_PIO_SM 0
#endif
#define BUS_PIO   WDC65C02_BUS_PIO_BLOCK
#define BUS_SM    WDC65C02_BUS_PIO_SM
static bool bus_open;         // Address taken, command not sent yet
static bool bus_rw_cycle;     // 65C02 read cycle
static uint8_t bus_wdata;     // Data of the last write cycle

static inline __attribute__((always_inline)) uint32_t bus_rx(void) {
    while (BUS_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + BUS_SM))) {
    }
    return BUS_PIO->rxf[BUS_SM];
}

static inline __attribute__((always_inline)) void bus_close(void) {
    if (bus_open) {
        bus_open = false;
        if (bus_rw_cycle) {
            BUS_PIO->txf[BUS_SM] = 0x1FF;   // Open bus
        } else {
            BUS_PIO->txf[BUS_SM] = 0;
            bus_wdata = (uint8_t)bus_rx();
        }
    }
}

static inline __attribute__((always_inline)) void bus_tick_pio(wdc6502cpu_t *c) {
    bus_close();
    uint32_t v = bus_rx();
    c->addr = (uint16_t)(((v >> 12) & 0xFF) | ((v & 0xFF) << 8));
    c->rw = (v >> _RW_PIN) & 1;
    bus_rw_cycle = c->rw;
    bus_open = true;
}

static inline __attribute__((always_inline)) uint8_t bus_get_data_pio(void) {
    if (bus_open) {
        bus_open = false;
        BUS_PIO->txf[BUS_SM] = 0;
        bus_wdata = (uint8_t)bus_rx();
    }
    return bus_wdata;
}

static inline __attribute__((always_inline)) void bus_set_data_pio(uint8_t data) {
    if (bus_open) {
        bus_open = false;
        BUS_PIO->txf[BUS_SM] = 0x100u | data;
    }
}

// Hand GPIO 0-10 and PHI2 over to the state machine (after wdc65C02cpu_init)
static void bus_pio_start(void) {
    uint offset = pio_add_program(BUS_PIO, &bus6502_program);
    pio_sm_config cfg = bus6502_program_get_default_config(offset);
    sm_config_set_in_pins(&cfg, 0);
    sm_config_set_out_pins(&cfg, 0, 8);
    sm_config_set_set_pins(&cfg, _CLOCK_PIN, 1);
    sm_config_set_sideset_pins(&cfg, _OE1_PIN);
    sm_config_set_in_shift(&cfg, false, false, 32);   // Shift left, no autopush
    sm_config_set_out_shift(&cfg, true, false, 32);   // Shift right, no autopull
    sm_config_set_clkdiv(&cfg, 1.0f);
    pio_sm_set_pins_with_mask(BUS_PIO, BUS_SM, (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN),
                              (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN));
    pio_sm_set_pindirs_with_mask(BUS_PIO, BUS_SM, (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN),
                                 0xFFu | (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN));
    for (uint pin = 0; pin <= _OE3_PIN; pin++) pio_gpio_init(BUS_PIO, pin);
    pio_gpio_init(BUS_PIO, _CLOCK_PIN);
    BUS_PIO->input_sync_bypass |= 0xFFFu;   // GPIO 0-11 sampled directly (stable after the delays)
    pio_sm_init(BUS_PIO, BUS_SM, offset + bus6502_offset_entry, &cfg);
    pio_sm_set_enabled(BUS_PIO, BUS_SM, true);
}

#define bus_tick     bus_tick_pio
#define bus_get_data bus_get_data_pio
#define bus_set_data bus_set_data_pio
#endif

#undef MOS6502CPU_TICK
#undef MOS6502CPU_GET_ADDR
#undef MOS6502CPU_GET_DATA
#undef MOS6502CPU_SET_DATA
#define MOS6502CPU_TICK(c)           bus_tick(c)
#define MOS6502CPU_GET_ADDR(c)       ((c)->addr)
#define MOS6502CPU_GET_DATA(c)       bus_get_data()
#define MOS6502CPU_SET_DATA(c, data) bus_set_data(data)
#endif
