#pragma once

// neo_esp_fake.h — faux modem « AT ESP8266 » (sous-ensemble UDP) pour les tests
//
// Reproduit le dialogue convenu avec Neo6502picowifi (2026-10-01) et ses chaînes
// actuelles (at_modem.c) : écho jusqu'à ATE0, OK / ERROR, AT+CIPSTART="UDP"
// (CONNECT, OK), AT+CIPSEND=n (\r\nOK\r\n> , puis Recv n bytes et SEND OK),
// réception \r\n+IPD,n: un datagramme à la fois, AT+CWJAP_DEF, AT+CIPCLOSE.
// Le transport UDP réel ou simulé est fourni par l'appelant.
//
// Utilisé par tests/oric/test_neo_esp.c et platforms/pc/netbridge/neo_esp_modem_pc.c.
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    // Octets vers l'hôte (la Neo6502)
    void (*out)(void* ctx, const uint8_t* p, int n);
    // Ouvre le lien UDP vers hôte:port ; envoie un datagramme
    bool (*udp_open)(void* ctx, const char* host, uint16_t port);
    bool (*udp_send)(void* ctx, const uint8_t* p, int n);
    void* ctx;
    bool echo;
    bool linked;
    char ssid[33];
    char line[200];
    int line_len;
    int send_expected, send_len;   // AT+CIPSEND en cours
    uint8_t send_buf[2048];
    uint32_t commands, sent, received;
} neo_esp_fake_t;

static inline void neo_esp_fake_init(neo_esp_fake_t* m) {
    m->echo = true;   // As an ESP8266 after reset
    m->linked = false;
    m->line_len = 0;
    m->send_expected = 0;
    m->ssid[0] = 0;
    m->commands = m->sent = m->received = 0;
}

static inline void _fake_out(neo_esp_fake_t* m, const char* s) { m->out(m->ctx, (const uint8_t*)s, (int)strlen(s)); }

// Copies a quoted argument ("…") from *p into out; false if absent
static inline bool _fake_quoted(const char** p, char* out, size_t n) {
    if (**p != '"') return false;
    const char* s = ++*p;
    const char* e = strchr(s, '"');
    if (!e || (size_t)(e - s) >= n) return false;
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = 0;
    *p = e + 1;
    return true;
}

static inline void _fake_command(neo_esp_fake_t* m, const char* cmd) {
    m->commands++;
    if (!strcmp(cmd, "AT")) {
        _fake_out(m, "\r\nOK\r\n");
    } else if (!strcmp(cmd, "ATE0")) {
        m->echo = false;
        _fake_out(m, "\r\nOK\r\n");
    } else if (!strncmp(cmd, "AT+CWJAP_DEF=", 13)) {
        const char* p = cmd + 13;
        char ssid[33], pass[65];
        if (!_fake_quoted(&p, ssid, sizeof(ssid)) || *p++ != ',' || !_fake_quoted(&p, pass, sizeof(pass))) {
            _fake_out(m, "\r\nERROR\r\n");
            return;
        }
        snprintf(m->ssid, sizeof(m->ssid), "%s", ssid);
        _fake_out(m, "WIFI CONNECTED\r\nWIFI GOT IP\r\n\r\nOK\r\n");
    } else if (!strcmp(cmd, "AT+CIPCLOSE")) {
        if (!m->linked) {
            _fake_out(m, "\r\nERROR\r\n");
            return;
        }
        m->linked = false;
        _fake_out(m, "CLOSED\r\n\r\nOK\r\n");
    } else if (!strncmp(cmd, "AT+CIPSTART=\"UDP\",", 18)) {
        const char* p = cmd + 18;
        char host[128];
        if (!_fake_quoted(&p, host, sizeof(host)) || *p++ != ',') {
            _fake_out(m, "\r\nERROR\r\n");
            return;
        }
        const int port = atoi(p);
        if (m->linked) {
            _fake_out(m, "ALREADY CONNECTED\r\n\r\nERROR\r\n");
            return;
        }
        if (port <= 0 || !m->udp_open(m->ctx, host, (uint16_t)port)) {
            _fake_out(m, "DNS Fail\r\n\r\nERROR\r\n");
            return;
        }
        m->linked = true;
        _fake_out(m, "CONNECT\r\n\r\nOK\r\n");
    } else if (!strncmp(cmd, "AT+CIPSEND=", 11)) {
        const int n = atoi(cmd + 11);
        if (n < 1 || n > (int)sizeof(m->send_buf)) {
            _fake_out(m, "\r\nERROR\r\n");
            return;
        }
        if (!m->linked) {
            _fake_out(m, "link is not valid\r\n\r\nERROR\r\n");
            return;
        }
        m->send_expected = n;
        m->send_len = 0;
        _fake_out(m, "\r\nOK\r\n> ");
    } else {
        _fake_out(m, "\r\nERROR\r\n");
    }
}

// Bytes from the host
static inline void neo_esp_fake_input(neo_esp_fake_t* m, const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (m->send_expected) {
            m->send_buf[m->send_len++] = c;
            if (m->send_len == m->send_expected) {
                m->send_expected = 0;
                const bool ok = m->udp_send(m->ctx, m->send_buf, m->send_len);
                char s[48];
                snprintf(s, sizeof(s), "\r\nRecv %d bytes\r\n", m->send_len);
                _fake_out(m, s);
                _fake_out(m, ok ? "\r\nSEND OK\r\n" : "\r\nSEND FAIL\r\n");
                m->sent++;
            }
            continue;
        }
        if (m->echo) m->out(m->ctx, &c, 1);
        if (c == '\r') continue;
        if (c == '\n') {
            m->line[m->line_len] = 0;
            if (m->line_len) _fake_command(m, m->line);
            m->line_len = 0;
            continue;
        }
        if (m->line_len < (int)sizeof(m->line) - 1) m->line[m->line_len++] = (char)c;
    }
}

// A datagram from the server: one +IPD
static inline void neo_esp_fake_udp_in(neo_esp_fake_t* m, const uint8_t* p, int n) {
    if (!m->linked) return;
    char s[24];
    snprintf(s, sizeof(s), "\r\n+IPD,%d:", n);
    _fake_out(m, s);
    m->out(m->ctx, p, n);
    m->received++;
}

// An unsolicited line (Wi-Fi events), as the modem prints them at any time
static inline void neo_esp_fake_unsolicited(neo_esp_fake_t* m, const char* line) {
    _fake_out(m, line);
    _fake_out(m, "\r\n");
}
