#pragma once

// neo_netlink.h — liaison série entre la Neo6502 et une passerelle réseau (Pico W)
//
// La Neo6502 n'a pas de réseau : un Pico W, branché sur son hub USB (port série
// USB) ou sur l'UEXT (UART), sert de passerelle Wi-Fi. Les deux échangent des
// trames sur la liaison série :
//
//   trame = SLIP( type, corps, CRC16-CCITT (type + corps), petit-boutiste )
//
// SLIP (RFC 1055 : END 0xC0, ESC 0xDB) délimite les trames et se resynchronise
// seul après une erreur ; le CRC écarte une trame abîmée (UART). Types :
//   NEO_NET_TNFS_REQ   Neo6502 → passerelle : un datagramme TNFS à envoyer au serveur
//   NEO_NET_TNFS_RESP  passerelle → Neo6502 : un datagramme reçu du serveur
//   NEO_NET_CONFIG     Neo6502 → passerelle : réseau Wi-Fi, mot de passe, serveur, port
//   NEO_NET_STATUS_REQ / NEO_NET_STATUS : état de la passerelle (Wi-Fi, adresses)
//   NEO_NET_LOG        passerelle → Neo6502 : texte de journal (ignoré par le client)
//
// Ce fichier contient le codage des trames, le transport du client TNFS
// (neo_netlink_xfer, pour neo_tnfs_init) et le cœur de la passerelle
// (neo_netbridge_*), commun au firmware du Pico W et à la passerelle PC des tests.
// C pur, sans allocation ; testé sur PC (tests/oric/test_neo_netlink.c).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEO_NET_FRAME_MAX 600   // Corps maximal (un datagramme TNFS : 532) + marge

enum {
    NEO_NET_TNFS_REQ = 0x01,
    NEO_NET_TNFS_RESP = 0x02,
    NEO_NET_CONFIG = 0x10,
    NEO_NET_STATUS_REQ = 0x11,
    NEO_NET_STATUS = 0x12,
    NEO_NET_LOG = 0x7F,
};

// État de la passerelle (premier octet de NEO_NET_STATUS)
enum { NEO_NET_IDLE = 0, NEO_NET_CONNECTING = 1, NEO_NET_UP = 2, NEO_NET_FAILED = 3 };

uint16_t neo_net_crc16(const uint8_t* p, int n);

// Code une trame dans out (taille max : 2 * (len + 3) + 2) ; renvoie sa taille
int neo_net_encode(uint8_t type, const uint8_t* body, int len, uint8_t* out);

// Décodeur incrémental : neo_net_feed renvoie true quand une trame complète et
// intacte est disponible (type, body, len) ; les trames abîmées sont comptées et ignorées
typedef struct {
    uint8_t buf[NEO_NET_FRAME_MAX + 3];
    int n;
    bool esc;
    bool overflow;
    uint8_t type;
    const uint8_t* body;
    int len;
    uint32_t frames, bad;
} neo_net_decoder_t;

void neo_net_decoder_init(neo_net_decoder_t* d);
bool neo_net_feed(neo_net_decoder_t* d, uint8_t c);

/*-- Côté Neo6502 : transport du client TNFS ---------------------------------*/
// Liaison série fournie par la plate-forme : écrire des octets ; lire un octet
// (renvoie -1 si rien n'arrive avant timeout_us)
typedef struct {
    bool (*write)(void* ctx, const uint8_t* p, int n);
    int (*read_byte)(void* ctx, uint32_t timeout_us);
    void* ctx;
    neo_net_decoder_t dec;
    uint8_t out[2 * (NEO_NET_FRAME_MAX + 3) + 2];
    uint32_t logs;   // Trames de journal reçues
} neo_netlink_t;

void neo_netlink_init(neo_netlink_t* l, bool (*write)(void*, const uint8_t*, int), int (*read_byte)(void*, uint32_t),
                      void* ctx);
bool neo_netlink_send(neo_netlink_t* l, uint8_t type, const uint8_t* body, int len);
// Attend une trame de ce type (les autres sont ignorées) ; renvoie la taille du corps copié, ou -1
int neo_netlink_wait(neo_netlink_t* l, uint8_t type, uint8_t* body, int max, uint32_t timeout_ms);
// Transport pour neo_tnfs_init(t, neo_netlink_xfer, &link)
int neo_netlink_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms);
// Configure la passerelle (Wi-Fi, serveur TNFS) ; puis neo_netlink_status jusqu'à NEO_NET_UP
bool neo_netlink_configure(neo_netlink_t* l, const char* ssid, const char* password, const char* server, uint16_t port);
// État de la passerelle (NEO_NET_*), -1 sans réponse ; ip / server_ip : 4 octets chacun (NULL possible)
int neo_netlink_status(neo_netlink_t* l, uint8_t* ip, uint8_t* server_ip, uint32_t timeout_ms);

/*-- Côté passerelle ----------------------------------------------------------*/
typedef struct {
    // Envoie des octets vers la Neo6502 (port série USB ou UART)
    void (*serial_write)(void* ctx, const uint8_t* p, int n);
    // Envoie un datagramme au serveur TNFS configuré ; false si le réseau n'est pas prêt
    bool (*udp_send)(void* ctx, const uint8_t* p, int n);
    // Nouvelle configuration (chaînes terminées par 0)
    void (*configure)(void* ctx, const char* ssid, const char* password, const char* server, uint16_t port);
    // État : NEO_NET_* et adresses (4 octets chacune)
    uint8_t (*status)(void* ctx, uint8_t* ip, uint8_t* server_ip);
    void* ctx;
    neo_net_decoder_t dec;
    uint8_t out[2 * (NEO_NET_FRAME_MAX + 3) + 2];
    uint32_t requests, replies, dropped;
} neo_netbridge_t;

void neo_netbridge_init(neo_netbridge_t* b);
// Octets reçus de la Neo6502
void neo_netbridge_serial_in(neo_netbridge_t* b, const uint8_t* p, int n);
// Datagramme reçu du serveur
void neo_netbridge_udp_in(neo_netbridge_t* b, const uint8_t* p, int n);
void neo_netbridge_log(neo_netbridge_t* b, const char* text);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef NEO_NETLINK_IMPL

#define _NEO_SLIP_END 0xC0
#define _NEO_SLIP_ESC 0xDB
#define _NEO_SLIP_ESC_END 0xDC
#define _NEO_SLIP_ESC_ESC 0xDD

uint16_t neo_net_crc16(const uint8_t* p, int n) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static int _neo_slip_put(uint8_t* out, int k, uint8_t c) {
    if (c == _NEO_SLIP_END) {
        out[k++] = _NEO_SLIP_ESC;
        out[k++] = _NEO_SLIP_ESC_END;
    } else if (c == _NEO_SLIP_ESC) {
        out[k++] = _NEO_SLIP_ESC;
        out[k++] = _NEO_SLIP_ESC_ESC;
    } else {
        out[k++] = c;
    }
    return k;
}

int neo_net_encode(uint8_t type, const uint8_t* body, int len, uint8_t* out) {
    if (len < 0 || len > NEO_NET_FRAME_MAX) return 0;
    uint16_t crc = 0xFFFF;
    int k = 0;
    out[k++] = _NEO_SLIP_END;   // Flushes any line noise before the frame
    // CRC over type + body, computed as the bytes go
    uint8_t t[1] = {type};
    crc = neo_net_crc16(t, 1);
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)(body[i] << 8);
        for (int b = 0; b < 8; b++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    k = _neo_slip_put(out, k, type);
    for (int i = 0; i < len; i++) k = _neo_slip_put(out, k, body[i]);
    k = _neo_slip_put(out, k, (uint8_t)crc);
    k = _neo_slip_put(out, k, (uint8_t)(crc >> 8));
    out[k++] = _NEO_SLIP_END;
    return k;
}

void neo_net_decoder_init(neo_net_decoder_t* d) { memset(d, 0, sizeof(*d)); }

bool neo_net_feed(neo_net_decoder_t* d, uint8_t c) {
    if (c == _NEO_SLIP_END) {
        const int n = d->n;
        const bool overflow = d->overflow;
        d->n = 0;
        d->esc = false;
        d->overflow = false;
        if (n == 0) return false;   // Empty frame: separator only
        if (overflow || n < 3 || neo_net_crc16(d->buf, n - 2) != (uint16_t)(d->buf[n - 2] | (d->buf[n - 1] << 8))) {
            d->bad++;
            return false;
        }
        d->type = d->buf[0];
        d->body = d->buf + 1;
        d->len = n - 3;
        d->frames++;
        return true;
    }
    if (c == _NEO_SLIP_ESC) {
        d->esc = true;
        return false;
    }
    if (d->esc) {
        d->esc = false;
        c = c == _NEO_SLIP_ESC_END ? _NEO_SLIP_END : c == _NEO_SLIP_ESC_ESC ? _NEO_SLIP_ESC : c;
    }
    if (d->n < (int)sizeof(d->buf)) {
        d->buf[d->n++] = c;
    } else {
        d->overflow = true;
    }
    return false;
}

void neo_netlink_init(neo_netlink_t* l, bool (*write)(void*, const uint8_t*, int), int (*read_byte)(void*, uint32_t),
                      void* ctx) {
    memset(l, 0, sizeof(*l));
    l->write = write;
    l->read_byte = read_byte;
    l->ctx = ctx;
    neo_net_decoder_init(&l->dec);
}

bool neo_netlink_send(neo_netlink_t* l, uint8_t type, const uint8_t* body, int len) {
    const int n = neo_net_encode(type, body, len, l->out);
    return n > 0 && l->write(l->ctx, l->out, n);
}

int neo_netlink_wait(neo_netlink_t* l, uint8_t type, uint8_t* body, int max, uint32_t timeout_ms) {
    // The timeout runs per byte: a frame that keeps arriving is waited for
    uint32_t left_us = timeout_ms * 1000u;
    while (left_us) {
        const uint32_t step = left_us < 10000u ? left_us : 10000u;
        const int c = l->read_byte(l->ctx, step);
        if (c < 0) {
            left_us -= step;
            continue;
        }
        if (!neo_net_feed(&l->dec, (uint8_t)c)) continue;
        if (l->dec.type == NEO_NET_LOG) l->logs++;
        if (l->dec.type != type) continue;
        if (l->dec.len > max) return -1;
        memcpy(body, l->dec.body, (size_t)l->dec.len);
        return l->dec.len;
    }
    return -1;
}

int neo_netlink_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms) {
    neo_netlink_t* l = (neo_netlink_t*)ctx;
    if (!neo_netlink_send(l, NEO_NET_TNFS_REQ, req, len)) return -1;
    return neo_netlink_wait(l, NEO_NET_TNFS_RESP, resp, max, timeout_ms);
}

bool neo_netlink_configure(neo_netlink_t* l, const char* ssid, const char* password, const char* server, uint16_t port) {
    uint8_t body[NEO_NET_FRAME_MAX];
    const size_t a = strlen(ssid) + 1, b = strlen(password) + 1, c = strlen(server) + 1;
    if (a + b + c + 2 > sizeof(body)) return false;
    memcpy(body, ssid, a);
    memcpy(body + a, password, b);
    memcpy(body + a + b, server, c);
    body[a + b + c] = (uint8_t)port;
    body[a + b + c + 1] = (uint8_t)(port >> 8);
    return neo_netlink_send(l, NEO_NET_CONFIG, body, (int)(a + b + c + 2));
}

int neo_netlink_status(neo_netlink_t* l, uint8_t* ip, uint8_t* server_ip, uint32_t timeout_ms) {
    uint8_t body[16];
    if (!neo_netlink_send(l, NEO_NET_STATUS_REQ, 0, 0)) return -1;
    const int n = neo_netlink_wait(l, NEO_NET_STATUS, body, sizeof(body), timeout_ms);
    if (n < 9) return -1;
    if (ip) memcpy(ip, body + 1, 4);
    if (server_ip) memcpy(server_ip, body + 5, 4);
    return body[0];
}

void neo_netbridge_init(neo_netbridge_t* b) {
    // Callbacks and ctx are set by the caller before or after: only the state is cleared
    neo_net_decoder_init(&b->dec);
    b->requests = b->replies = b->dropped = 0;
}

static void _neo_netbridge_send(neo_netbridge_t* b, uint8_t type, const uint8_t* body, int len) {
    const int n = neo_net_encode(type, body, len, b->out);
    if (n > 0) b->serial_write(b->ctx, b->out, n);
}

void neo_netbridge_log(neo_netbridge_t* b, const char* text) {
    _neo_netbridge_send(b, NEO_NET_LOG, (const uint8_t*)text, (int)strlen(text));
}

static void _neo_netbridge_frame(neo_netbridge_t* b) {
    const uint8_t* p = b->dec.body;
    const int n = b->dec.len;
    switch (b->dec.type) {
        case NEO_NET_TNFS_REQ:
            b->requests++;
            if (!b->udp_send(b->ctx, p, n)) b->dropped++;   // The client resends after its timeout
            break;
        case NEO_NET_CONFIG: {
            // ssid \0 password \0 server \0 port(2)
            const char* s[3];
            int k = 0;
            for (int i = 0; i < 3; i++) {
                s[i] = (const char*)p + k;
                while (k < n && p[k]) k++;
                if (k >= n) return;   // Malformed: ignored
                k++;
            }
            if (k + 2 > n) return;
            if (b->configure) b->configure(b->ctx, s[0], s[1], s[2], (uint16_t)(p[k] | (p[k + 1] << 8)));
            break;
        }
        case NEO_NET_STATUS_REQ: {
            uint8_t body[9] = {NEO_NET_IDLE};
            if (b->status) body[0] = b->status(b->ctx, body + 1, body + 5);
            _neo_netbridge_send(b, NEO_NET_STATUS, body, sizeof(body));
            break;
        }
        default:
            break;
    }
}

void neo_netbridge_serial_in(neo_netbridge_t* b, const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) {
        if (neo_net_feed(&b->dec, p[i])) _neo_netbridge_frame(b);
    }
}

void neo_netbridge_udp_in(neo_netbridge_t* b, const uint8_t* p, int n) {
    b->replies++;
    _neo_netbridge_send(b, NEO_NET_TNFS_RESP, p, n);
}

#endif  // NEO_NETLINK_IMPL
