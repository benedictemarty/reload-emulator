// test_neo_esp.c — transport TNFS par modem AT ESP8266 (src/devices/neo_esp_at.h)
//
// Client TNFS → neo_esp_xfer → octets série → faux modem (platforms/pc/neo_esp_fake.h,
// dialogue de Neo6502picowifi) → faux serveur TNFS → +IPD → client. Écho avant ATE0,
// lignes spontanées, réponse +IPD avant ou après SEND OK, SEND FAIL.

#include <stdio.h>
#include <string.h>

#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#define NEO_TNFS_IMPL
#include "devices/neo_tnfs.h"
#define NEO_ESP_AT_IMPL
#include "devices/neo_esp_at.h"
#include "../../platforms/pc/neo_esp_fake.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static neo_esp_fake_t modem;
static uint8_t to_host[16384];
static int head, tail;
static void modem_out(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) to_host[tail++ % sizeof(to_host)] = p[i];
}

static char udp_host[64];
static uint16_t udp_port;
static bool udp_open(void* ctx, const char* host, uint16_t port) {
    (void)ctx;
    snprintf(udp_host, sizeof(udp_host), "%s", host);
    udp_port = port;
    return strcmp(host, "nowhere") != 0;
}

// Fake TNFS server: MOUNT and STAT; the answer is sent at once (inside CIPSEND:
// +IPD before SEND OK) or after the SEND OK (as the Pico W does)
static bool answer_early, fail_send, noise;
static uint8_t pending[64];
static int pending_len;
static bool udp_send(void* ctx, const uint8_t* req, int n) {
    (void)ctx;
    (void)n;
    if (fail_send) return false;
    uint8_t* r = pending;
    memset(r, 0, sizeof(pending));
    r[0] = req[3] == NEO_TNFS_MOUNT ? 0xEF : req[0];
    r[1] = req[3] == NEO_TNFS_MOUNT ? 0xBE : req[1];
    r[2] = req[2];
    r[3] = req[3];
    pending_len = 5;
    if (req[3] == NEO_TNFS_MOUNT) {
        r[5] = 2; r[6] = 1;
        pending_len = 9;
    } else if (req[3] == NEO_TNFS_STAT) {
        r[5] = 0xA4; r[6] = 0x81; r[11] = 0x00; r[12] = 0x10;   // Size 4096
        pending_len = 25;
    } else {
        r[4] = 0x16;
    }
    if (answer_early) {
        neo_esp_fake_udp_in(&modem, pending, pending_len);
        pending_len = 0;
    }
    return true;
}

// Host side
static bool host_write(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    neo_esp_fake_input(&modem, p, n);
    if (pending_len) {   // The Pico W answers after its SEND OK
        if (noise) neo_esp_fake_unsolicited(&modem, "WIFI GOT IP");
        neo_esp_fake_udp_in(&modem, pending, pending_len);
        pending_len = 0;
    }
    return true;
}
static int host_read(void* ctx, uint32_t timeout_us) {
    (void)ctx;
    (void)timeout_us;
    return head == tail ? -1 : to_host[head++ % sizeof(to_host)];
}

int main(void) {
    modem.out = modem_out;
    modem.udp_open = udp_open;
    modem.udp_send = udp_send;
    neo_esp_fake_init(&modem);
    static neo_esp_t e;
    neo_esp_init(&e, host_write, host_read, 0);

    // Opening: echo on until ATE0, Wi-Fi joined, UDP link
    neo_esp_fake_unsolicited(&modem, "WIFI CONNECTED");   // Already in the buffer before AT
    CHECK(neo_esp_open_udp(&e, "192.168.1.10", 16384, "MaBox", "secret"));
    CHECK(!modem.echo && !strcmp(modem.ssid, "MaBox") && modem.linked);
    CHECK(!strcmp(udp_host, "192.168.1.10") && udp_port == 16384);
    // TNFS through the modem, answer after SEND OK, with an unsolicited line in between
    static neo_tnfs_t t;
    neo_tnfs_init(&t, neo_esp_xfer, &e);
    noise = true;
    CHECK(neo_tnfs_mount(&t, "/", 0, 0) && t.session == 0xBEEF);
    noise = false;
    uint32_t size = 0;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0) && size == 4096);
    // Answer before SEND OK (kept in the queue)
    answer_early = true;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0) && size == 4096);
    answer_early = false;
    CHECK(t.resent == 0 && e.dropped == 0);
    // SEND FAIL: the TNFS client tries again, then gives up
    fail_send = true;
    CHECK(!neo_tnfs_stat(&t, "/A.DSK", &size, 0) && e.send_fail == (uint32_t)t.tries);
    fail_send = false;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0));
    // Unknown host: CIPSTART fails
    static neo_esp_t e2;
    neo_esp_fake_init(&modem);
    neo_esp_init(&e2, host_write, host_read, 0);
    CHECK(!neo_esp_open_udp(&e2, "nowhere", 16384, 0, 0) && !e2.open);
    CHECK(neo_esp_xfer(&e2, (const uint8_t*)"x", 1, (uint8_t*)udp_host, 8, 10) == -1);
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
