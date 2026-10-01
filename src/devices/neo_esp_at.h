#pragma once

// neo_esp_at.h — transport UDP du client TNFS par un modem Wi-Fi « AT ESP8266 »
//
// Le modem est Neo6502picowifi (Pico W, ~/Neo6502picowifi, UDP au format
// ESP8266 convenu le 2026-10-01) ou un vrai module ESP8266 (MOD-WIFI-ESP8266
// sur l'UEXT), branché sur un port série USB ou une UART. Dialogue utilisé :
//
//   AT, ATE0, AT+CIPCLOSE (ERROR toléré), [AT+CWJAP_DEF="réseau","mot de passe"] (35 s),
//   AT+CIPSTATUS jusqu'à STATUS:2/3/4 (adresse IP obtenue ; le modem rejoint son
//   réseau enregistré en tâche de fond après son branchement, jusqu'à 30 s),
//   AT+CIPSTART="UDP","hôte",port            → CONNECT, OK (12 s : DNS jusqu'à 10 s)
//   AT+CIPSEND=n                             → OK, « > » ; puis n octets
//                                            → Recv n bytes, SEND OK | SEND FAIL
//   réception : \r\n+IPD,n: suivi de n octets, un par datagramme
//
// Les lignes spontanées (WIFI CONNECTED, WIFI GOT IP…) et l'écho sont ignorés ;
// un +IPD arrivé pendant l'attente du « > » ou du SEND OK est gardé (file de 2).
//
//   neo_esp_init(&e, write, read_byte, ctx);
//   neo_esp_open_udp(&e, "192.168.1.10", 16384, NULL, NULL);
//   neo_tnfs_init(&tnfs, neo_esp_xfer, &e);
//
// C pur, sans allocation ; testé sur PC contre un faux modem
// (tests/oric/test_neo_esp.c, platforms/pc/neo_esp_fake.h).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEO_ESP_DGRAM_MAX 600   // Datagramme TNFS (532) + marge
#define NEO_ESP_LINE_MAX  96

// Fin de commande vue par l'analyseur
enum {
    NEO_ESP_NONE = 0,
    NEO_ESP_OK = 1,
    NEO_ESP_ERROR = 2,       // ERROR, FAIL
    NEO_ESP_SEND_OK = 3,
    NEO_ESP_SEND_FAIL = 4,
};

typedef struct {
    bool (*write)(void* ctx, const uint8_t* p, int n);
    int (*read_byte)(void* ctx, uint32_t timeout_us);   // -1 si rien avant timeout_us
    void* ctx;
    // Analyseur
    char line[NEO_ESP_LINE_MAX];
    int line_len;
    int ipd;                 // 0 : texte ; 1 : longueur de +IPD ; 2 : données
    uint32_t ipd_len, ipd_got;   // Received straight into the free queue slot
    bool prompt;             // « > » reçu
    int result;              // NEO_ESP_*
    int status;              // Dernier STATUS:n de AT+CIPSTATUS (-1 : aucun)
    // Datagrammes reçus en attente
    uint8_t queue[2][NEO_ESP_DGRAM_MAX];
    int queue_len[2];
    int queued;
    bool open;
    uint32_t datagrams, dropped, send_fail;
} neo_esp_t;

void neo_esp_init(neo_esp_t* e, bool (*write)(void*, const uint8_t*, int), int (*read_byte)(void*, uint32_t), void* ctx);
// Commande (sans \r\n) ; true sur OK, false sur ERROR / FAIL ou sans réponse
bool neo_esp_command(neo_esp_t* e, const char* cmd, uint32_t timeout_ms);
// Synchronise le modem (AT, ATE0), rejoint le réseau si ssid est donné, attend une adresse IP
bool neo_esp_prepare(neo_esp_t* e, const char* ssid, const char* password);
// Prépare le modem et ouvre le lien UDP ; ssid NULL : réseau Wi-Fi enregistré dans le modem
bool neo_esp_open_udp(neo_esp_t* e, const char* host, uint16_t port, const char* ssid, const char* password);
// Prépare le modem et donne le serveur TNFS à son second port série (AT$TNFS="hôte",port :
// Neo6502picowifi eeda7d3 ; PicoWiFiModemUSB, convenu) ; les datagrammes passent ensuite
// par ce second port (src/devices/neo_dgram_serial.h)
bool neo_esp_set_tnfs(neo_esp_t* e, const char* host, uint16_t port, const char* ssid, const char* password);
// Transport pour neo_tnfs_init(t, neo_esp_xfer, &e)
int neo_esp_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef NEO_ESP_AT_IMPL

void neo_esp_init(neo_esp_t* e, bool (*write)(void*, const uint8_t*, int), int (*read_byte)(void*, uint32_t), void* ctx) {
    memset(e, 0, sizeof(*e));
    e->write = write;
    e->read_byte = read_byte;
    e->ctx = ctx;
}

static void _neo_esp_line(neo_esp_t* e) {
    const char* l = e->line;
    if (!strncmp(l, "STATUS:", 7) && l[7] >= '0' && l[7] <= '9') e->status = l[7] - '0';
    if (!strcmp(l, "OK")) e->result = NEO_ESP_OK;
    else if (!strcmp(l, "SEND OK")) e->result = NEO_ESP_SEND_OK;
    else if (!strcmp(l, "SEND FAIL")) e->result = NEO_ESP_SEND_FAIL;
    else if (!strcmp(l, "ERROR") || !strcmp(l, "FAIL")) e->result = NEO_ESP_ERROR;
    // Anything else (echo, CONNECT, Recv n bytes, WIFI GOT IP…) is information only
}

static void _neo_esp_feed(neo_esp_t* e, uint8_t c) {
    if (e->ipd == 2) {
        // Into the next free slot; a datagram that is too long, or with the queue
        // full, is read to its end and dropped
        const bool keep = e->queued < 2 && e->ipd_len <= NEO_ESP_DGRAM_MAX;
        if (keep) e->queue[e->queued][e->ipd_got] = c;
        if (++e->ipd_got == e->ipd_len) {
            e->ipd = 0;
            e->datagrams++;
            if (keep) {
                e->queue_len[e->queued++] = (int)e->ipd_len;
            } else {
                e->dropped++;
            }
        }
        return;
    }
    if (e->ipd == 1) {
        if (c >= '0' && c <= '9' && e->ipd_len < 100000) {
            e->ipd_len = e->ipd_len * 10 + (uint32_t)(c - '0');
        } else if (c == ':' && e->ipd_len > 0) {
            e->ipd = 2;
            e->ipd_got = 0;
        } else {
            e->ipd = 0;   // Not a +IPD after all
        }
        return;
    }
    if (c == '\r') return;
    if (c == '\n') {
        e->line[e->line_len] = 0;
        if (e->line_len) _neo_esp_line(e);
        e->line_len = 0;
        return;
    }
    if (e->line_len < NEO_ESP_LINE_MAX - 1) e->line[e->line_len++] = (char)c;
    if (e->line_len == 1 && c == '>') {
        e->prompt = true;   // "> ": no end of line follows the prompt
        e->line_len = 0;
    } else if (e->line_len == 5 && !memcmp(e->line, "+IPD,", 5)) {
        e->ipd = 1;
        e->ipd_len = 0;
        e->line_len = 0;
    }
}

// Reads until done(e) or timeout; true if done
static bool _neo_esp_wait(neo_esp_t* e, bool (*done)(const neo_esp_t*), uint32_t timeout_ms) {
    uint32_t left_us = timeout_ms * 1000u;
    while (!done(e)) {
        if (!left_us) return false;
        const uint32_t step = left_us < 10000u ? left_us : 10000u;
        const int c = e->read_byte(e->ctx, step);
        if (c < 0) {
            left_us -= step;
            continue;
        }
        _neo_esp_feed(e, (uint8_t)c);
    }
    return true;
}

static bool _neo_esp_has_result(const neo_esp_t* e) { return e->result != NEO_ESP_NONE; }
static bool _neo_esp_has_prompt(const neo_esp_t* e) { return e->prompt || e->result == NEO_ESP_ERROR; }
static bool _neo_esp_sent(const neo_esp_t* e) {
    return e->result == NEO_ESP_SEND_OK || e->result == NEO_ESP_SEND_FAIL || e->result == NEO_ESP_ERROR;
}
static bool _neo_esp_has_dgram(const neo_esp_t* e) { return e->queued > 0; }

static bool _neo_esp_send_str(neo_esp_t* e, const char* s) { return e->write(e->ctx, (const uint8_t*)s, (int)strlen(s)); }

bool neo_esp_command(neo_esp_t* e, const char* cmd, uint32_t timeout_ms) {
    e->result = NEO_ESP_NONE;
    if (!_neo_esp_send_str(e, cmd) || !_neo_esp_send_str(e, "\r\n")) return false;
    return _neo_esp_wait(e, _neo_esp_has_result, timeout_ms) && e->result == NEO_ESP_OK;
}

bool neo_esp_prepare(neo_esp_t* e, const char* ssid, const char* password) {
    bool sync = false;
    for (int i = 0; i < 3 && !sync; i++) sync = neo_esp_command(e, "AT", 1000);
    if (!sync) return false;
    neo_esp_command(e, "ATE0", 1000);
    char cmd[160];
    if (ssid && ssid[0]) {
        if (snprintf(cmd, sizeof(cmd), "AT+CWJAP_DEF=\"%s\",\"%s\"", ssid, password ? password : "") >= (int)sizeof(cmd)) {
            return false;
        }
        if (!neo_esp_command(e, cmd, 35000)) return false;   // The modem retries up to 30 s (Neo6502picowifi)
    }
    // Wait for an IP address (STATUS:2 got IP, 3 connected, 4 disconnected link): up to 30 s
    bool ip = false;
    for (int i = 0; i < 60 && !ip; i++) {
        e->status = -1;
        neo_esp_command(e, "AT+CIPSTATUS", 1000);
        ip = e->status >= 2 && e->status <= 4;
        if (!ip) {
            // About 500 ms between polls: read (and drop) what comes meanwhile
            for (int k = 0; k < 50; k++) e->read_byte(e->ctx, 10000);
        }
    }
    return ip;
}

bool neo_esp_set_tnfs(neo_esp_t* e, const char* host, uint16_t port, const char* ssid, const char* password) {
    if (!neo_esp_prepare(e, ssid, password)) return false;
    char cmd[160];
    if (snprintf(cmd, sizeof(cmd), "AT$TNFS=\"%s\",%u", host, (unsigned)port) >= (int)sizeof(cmd)) return false;
    return neo_esp_command(e, cmd, 2000);
}

bool neo_esp_open_udp(neo_esp_t* e, const char* host, uint16_t port, const char* ssid, const char* password) {
    if (!neo_esp_prepare(e, ssid, password)) return false;
    char cmd[160];
    neo_esp_command(e, "AT+CIPCLOSE", 1000);   // ERROR when no link is open: fine
    if (snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"UDP\",\"%s\",%u", host, (unsigned)port) >= (int)sizeof(cmd)) return false;
    e->open = neo_esp_command(e, cmd, 12000);   // A host name: DNS up to 10 s in the modem
    e->queued = 0;
    return e->open;
}

int neo_esp_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms) {
    neo_esp_t* e = (neo_esp_t*)ctx;
    if (!e->open || len < 1 || len > NEO_ESP_DGRAM_MAX) return -1;
    e->queued = 0;   // Late answers to earlier requests: the TNFS client resends anyway
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", len);
    e->prompt = false;
    e->result = NEO_ESP_NONE;
    if (!_neo_esp_send_str(e, cmd) || !_neo_esp_wait(e, _neo_esp_has_prompt, 1000) || !e->prompt) return -1;
    e->result = NEO_ESP_NONE;
    if (!e->write(e->ctx, req, len) || !_neo_esp_wait(e, _neo_esp_sent, 2000) || e->result != NEO_ESP_SEND_OK) {
        e->send_fail++;
        return -1;
    }
    if (!_neo_esp_wait(e, _neo_esp_has_dgram, timeout_ms)) return -1;
    const int n = e->queue_len[0];
    memcpy(resp, e->queue[0], (size_t)(n < max ? n : max));
    e->queued--;
    if (e->queued) {
        memcpy(e->queue[0], e->queue[1], (size_t)e->queue_len[1]);
        e->queue_len[0] = e->queue_len[1];
    }
    return n <= max ? n : -1;
}

#endif  // NEO_ESP_AT_IMPL
