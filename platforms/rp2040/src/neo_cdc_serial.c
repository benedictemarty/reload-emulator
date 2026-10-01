// neo_cdc_serial.c — ports série USB côté hôte (TinyUSB CDC) pour les transports réseau
//
// Un modem Wi-Fi Pico W branché sur le hub USB de la Neo6502 est vu comme un ou
// deux ports série CDC, numérotés par ordre d'interface : port 0 = le modem AT
// (Neo6502picowifi, PicoWiFiModemUSB), port 1 = le relais TNFS proposé pour
// PicoWiFiModemUSB (docs/RESEAU-NEO6502.md). Les fonctions ont la forme attendue par
// neo_esp_init (src/devices/neo_esp_at.h) et neo_dgram_init (neo_dgram_serial.h) ;
// ctx = numéro de port ((void *)0 ou (void *)1). Le firmware doit compiler TinyUSB
// avec CFG_TUH_CDC ≥ 1 (2 pour les deux ports).
//
//   if (neo_cdc_ready()) neo_esp_init(&esp, neo_cdc_write, neo_cdc_read_byte, (void *)0);
//
// Les fonctions font tourner tuh_task pendant leurs attentes : à appeler depuis la
// boucle principale (pas depuis un rappel de TinyUSB).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/time.h"
#include "tusb.h"

#define NEO_CDC_PORTS 2

// CDC interfaces of the serial device, sorted by interface number (-1: none)
static volatile int neo_cdc_idx[NEO_CDC_PORTS] = {-1, -1};
static volatile uint8_t neo_cdc_itf[NEO_CDC_PORTS];
static volatile uint8_t neo_cdc_daddr;
static volatile uint32_t neo_cdc_mounts;   // Incremented at each mount (a modem plugged in again)

void tuh_cdc_mount_cb(uint8_t idx) {
    tuh_itf_info_t info;
    if (!tuh_cdc_itf_get_info(idx, &info)) return;
    if (neo_cdc_idx[0] >= 0 && info.daddr != neo_cdc_daddr) return;   // Ports of the first serial device only
    neo_cdc_daddr = info.daddr;
    const uint8_t itf = info.desc.bInterfaceNumber;
    // Insert in interface order
    int p = 0;
    while (p < NEO_CDC_PORTS && neo_cdc_idx[p] >= 0 && neo_cdc_itf[p] < itf) p++;
    if (p == NEO_CDC_PORTS) return;
    for (int k = NEO_CDC_PORTS - 1; k > p; k--) {
        neo_cdc_idx[k] = neo_cdc_idx[k - 1];
        neo_cdc_itf[k] = neo_cdc_itf[k - 1];
    }
    neo_cdc_idx[p] = idx;
    neo_cdc_itf[p] = itf;
    neo_cdc_mounts++;
    printf("CDC: serial interface %u (USB %u) mounted as port %d\n", itf, info.daddr, p);
}

void tuh_cdc_umount_cb(uint8_t idx) {
    for (int p = 0; p < NEO_CDC_PORTS; p++) {
        if (neo_cdc_idx[p] == idx) {
            for (int k = p; k < NEO_CDC_PORTS - 1; k++) {
                neo_cdc_idx[k] = neo_cdc_idx[k + 1];
                neo_cdc_itf[k] = neo_cdc_itf[k + 1];
            }
            neo_cdc_idx[NEO_CDC_PORTS - 1] = -1;
            printf("CDC: serial interface removed (port %d)\n", p);
            return;
        }
    }
}

static int neo_cdc_port_idx(int port) {
    if (port < 0 || port >= NEO_CDC_PORTS) return -1;
    const int idx = neo_cdc_idx[port];
    return idx >= 0 && tuh_cdc_mounted((uint8_t)idx) ? idx : -1;
}

bool neo_cdc_port_ready(int port) { return neo_cdc_port_idx(port) >= 0; }
bool neo_cdc_ready(void) { return neo_cdc_port_ready(0); }
uint32_t neo_cdc_mount_count(void) { return neo_cdc_mounts; }

bool neo_cdc_write(void *ctx, const uint8_t *p, int n) {
    const int port = (int)(intptr_t)ctx;
    const uint32_t start = time_us_32();
    while (n > 0) {
        const int idx = neo_cdc_port_idx(port);
        if (idx < 0) return false;
        const uint32_t k = tuh_cdc_write((uint8_t)idx, p, (uint32_t)n);
        p += k;
        n -= (int)k;
        tuh_cdc_write_flush((uint8_t)idx);
        if (n > 0) {
            tuh_task();   // FIFO full: let the transfers go
            if (time_us_32() - start > 1000000u) return false;
        }
    }
    return true;
}

int neo_cdc_read_byte(void *ctx, uint32_t timeout_us) {
    const int port = (int)(intptr_t)ctx;
    const uint32_t start = time_us_32();
    for (;;) {
        const int idx = neo_cdc_port_idx(port);
        if (idx < 0) return -1;
        uint8_t c;
        if (tuh_cdc_read_available((uint8_t)idx) && tuh_cdc_read((uint8_t)idx, &c, 1) == 1) return c;
        if (time_us_32() - start >= timeout_us) return -1;
        tuh_task();
    }
}
