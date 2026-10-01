// neo_cdc_serial.c — port série USB côté hôte (TinyUSB CDC) pour neo_esp_at
//
// Le modem Wi-Fi Neo6502picowifi (Pico W) branché sur le hub USB de la Neo6502
// est vu comme un port série CDC ; ces fonctions ont la forme attendue par
// neo_esp_init (src/devices/neo_esp_at.h). Le firmware doit compiler TinyUSB avec
// CFG_TUH_CDC=1 (tusb_config.h du socle, réglable).
//
//   if (neo_cdc_ready()) neo_esp_init(&esp, neo_cdc_write, neo_cdc_read_byte, 0);
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

static volatile int neo_cdc_idx = -1;   // Interface CDC montée, -1 : aucune
static volatile uint32_t neo_cdc_serial_n;   // Incrémenté à chaque montage (détecter un modem rebranché)

void tuh_cdc_mount_cb(uint8_t idx) {
    if (neo_cdc_idx < 0) {
        neo_cdc_idx = idx;
        neo_cdc_serial_n++;
        printf("CDC: serial device %u mounted\n", idx);
    }
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (neo_cdc_idx == idx) {
        neo_cdc_idx = -1;
        printf("CDC: serial device %u removed\n", idx);
    }
}

bool neo_cdc_ready(void) { return neo_cdc_idx >= 0 && tuh_cdc_mounted((uint8_t)neo_cdc_idx); }
uint32_t neo_cdc_mount_count(void) { return neo_cdc_serial_n; }

bool neo_cdc_write(void *ctx, const uint8_t *p, int n) {
    (void)ctx;
    const uint32_t start = time_us_32();
    while (n > 0) {
        if (!neo_cdc_ready()) return false;
        const uint32_t k = tuh_cdc_write((uint8_t)neo_cdc_idx, p, (uint32_t)n);
        p += k;
        n -= (int)k;
        tuh_cdc_write_flush((uint8_t)neo_cdc_idx);
        if (n > 0) {
            tuh_task();   // FIFO full: let the transfers go
            if (time_us_32() - start > 1000000u) return false;
        }
    }
    return true;
}

int neo_cdc_read_byte(void *ctx, uint32_t timeout_us) {
    (void)ctx;
    const uint32_t start = time_us_32();
    for (;;) {
        if (!neo_cdc_ready()) return -1;
        uint8_t c;
        if (tuh_cdc_read_available((uint8_t)neo_cdc_idx) && tuh_cdc_read((uint8_t)neo_cdc_idx, &c, 1) == 1) return c;
        if (time_us_32() - start >= timeout_us) return -1;
        tuh_task();
    }
}
