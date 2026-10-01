#pragma once

// neo_dgram_serial.h — transport du client TNFS : un datagramme par trame sur une
// liaison série, précédé de sa longueur
//
// Pour un relais UDP dédié, par exemple le second port série USB proposé pour
// PicoWiFiModemUSB (décision de bmarty, 2026-10-01 : un seul Pico W pour le Minitel
// sur le premier port et TNFS sur le second). Trame, dans les deux sens :
//
//   longueur (2 octets, petit-boutiste, 1 à NEO_DGRAM_MAX) | datagramme
//
// Le relais envoie chaque trame reçue en UDP au serveur TNFS et renvoie chaque
// datagramme reçu dans une trame. Format PROPOSÉ (ordre des octets, configuration
// du serveur) : à confirmer avec la session du firmware (docs/RESEAU-NEO6502.md).
//
// Une longueur hors limites (désynchronisation) fait vider ce qui arrive jusqu'à
// un silence, et la requête est perdue : le client TNFS la renvoie.
//
// C pur ; testé sur PC (tests/oric/test_neo_dgram.c).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEO_DGRAM_MAX 600

typedef struct {
    bool (*write)(void* ctx, const uint8_t* p, int n);
    int (*read_byte)(void* ctx, uint32_t timeout_us);   // -1 si rien avant timeout_us
    void* ctx;
    uint32_t sent, received, resync, stale;
} neo_dgram_t;

void neo_dgram_init(neo_dgram_t* d, bool (*write)(void*, const uint8_t*, int), int (*read_byte)(void*, uint32_t),
                    void* ctx);
// Transport pour neo_tnfs_init(t, neo_dgram_xfer, &d)
int neo_dgram_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef NEO_DGRAM_SERIAL_IMPL

void neo_dgram_init(neo_dgram_t* d, bool (*write)(void*, const uint8_t*, int), int (*read_byte)(void*, uint32_t),
                    void* ctx) {
    memset(d, 0, sizeof(*d));
    d->write = write;
    d->read_byte = read_byte;
    d->ctx = ctx;
}

// Drops whatever is pending until the line is quiet (after a desynchronisation)
static void _neo_dgram_drain(neo_dgram_t* d) {
    d->resync++;
    while (d->read_byte(d->ctx, 20000) >= 0) {}
}

// Reads one frame (length then bytes); first_us: wait for its first byte
static int _neo_dgram_frame(neo_dgram_t* d, uint8_t* buf, int max, uint32_t first_us) {
    const int lo = d->read_byte(d->ctx, first_us);
    if (lo < 0) return -1;
    const int hi = d->read_byte(d->ctx, 100000);
    if (hi < 0) return -1;
    const int n = lo | (hi << 8);
    if (n < 1 || n > NEO_DGRAM_MAX) {
        _neo_dgram_drain(d);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        const int c = d->read_byte(d->ctx, 100000);
        if (c < 0) return -1;   // Truncated frame
        if (buf && i < max) buf[i] = (uint8_t)c;
    }
    return n;
}

int neo_dgram_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms) {
    neo_dgram_t* d = (neo_dgram_t*)ctx;
    if (len < 1 || len > NEO_DGRAM_MAX) return -1;
    // Late answers to earlier requests (after a timeout) are dropped first: read as
    // the answer to this request, they would put every exchange one frame behind
    while (_neo_dgram_frame(d, 0, 0, 0) > 0) d->stale++;
    const uint8_t hdr[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
    if (!d->write(d->ctx, hdr, 2) || !d->write(d->ctx, req, len)) return -1;
    d->sent++;
    // Length: waited for up to timeout_ms; then the bytes of the frame follow closely
    const int n = _neo_dgram_frame(d, resp, max, timeout_ms * 1000u);
    if (n < 0) return -1;   // The TNFS client resends
    d->received++;
    return n <= max ? n : -1;
}

#endif  // NEO_DGRAM_SERIAL_IMPL
