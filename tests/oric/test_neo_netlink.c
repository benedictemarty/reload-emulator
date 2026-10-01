// test_neo_netlink.c — liaison série Neo6502 / passerelle (src/devices/neo_netlink.h)
//
// Trames SLIP + CRC (octets spéciaux, bruit, trame abîmée, débordement), puis la
// chaîne complète en mémoire : client TNFS → neo_netlink_xfer → octets série →
// cœur de la passerelle → faux serveur TNFS → retour.

#include <stdio.h>
#include <string.h>

#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#define NEO_TNFS_IMPL
#include "devices/neo_tnfs.h"
#define NEO_NETLINK_IMPL
#include "devices/neo_netlink.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static void test_frames(void) {
    CHECK(neo_net_crc16((const uint8_t*)"123456789", 9) == 0x29B1);   // CRC-16/CCITT-FALSE check value
    uint8_t body[300], out[2 * 303 + 2];
    for (int i = 0; i < 300; i++) body[i] = (uint8_t)i;
    body[0] = 0xC0;
    body[1] = 0xDB;   // SLIP END and ESC inside the data
    const int n = neo_net_encode(NEO_NET_TNFS_REQ, body, 300, out);
    CHECK(n > 300 && out[0] == 0xC0 && out[n - 1] == 0xC0);
    int ends = 0;
    for (int i = 1; i < n - 1; i++) ends += out[i] == 0xC0;
    CHECK(ends == 0);   // No END inside the frame
    neo_net_decoder_t d;
    neo_net_decoder_init(&d);
    neo_net_feed(&d, 0x41);   // Line noise before the frame
    neo_net_feed(&d, 0x42);
    int got = 0;
    for (int i = 0; i < n; i++) got += neo_net_feed(&d, out[i]);
    CHECK(got == 1 && d.type == NEO_NET_TNFS_REQ && d.len == 300 && !memcmp(d.body, body, 300));
    CHECK(d.bad == 1);   // The noise, ended by the END that opens the frame
    // A damaged byte: frame dropped, the next one decoded
    out[10] ^= 0x01;
    got = 0;
    for (int i = 0; i < n; i++) got += neo_net_feed(&d, out[i]);
    CHECK(got == 0 && d.bad == 2);
    out[10] ^= 0x01;
    got = 0;
    for (int i = 0; i < n; i++) got += neo_net_feed(&d, out[i]);
    CHECK(got == 1 && d.len == 300);
    // Overflow: too long, dropped, then back in step
    for (int i = 0; i < NEO_NET_FRAME_MAX + 50; i++) neo_net_feed(&d, 0x55);
    CHECK(!neo_net_feed(&d, 0xC0) && d.bad == 3);
    const int m = neo_net_encode(NEO_NET_LOG, (const uint8_t*)"ok", 2, out);
    got = 0;
    for (int i = 0; i < m; i++) got += neo_net_feed(&d, out[i]);
    CHECK(got == 1 && d.type == NEO_NET_LOG && d.len == 2);
    CHECK(neo_net_encode(1, body, NEO_NET_FRAME_MAX + 1, out) == 0);
}

/*-- In-memory serial link and bridge ------------------------------------------*/
static uint8_t to_host[8192];
static int to_host_head, to_host_tail;
static neo_netbridge_t bridge;

static void bridge_serial_write(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) to_host[to_host_tail++ % sizeof(to_host)] = p[i];
}

// Fake TNFS server behind the bridge: MOUNT and STAT only
static int udp_sent;
static bool bridge_udp_send(void* ctx, const uint8_t* req, int n) {
    (void)ctx;
    (void)n;
    udp_sent++;
    uint8_t r[32] = {0};
    int len = 5;
    r[0] = req[3] == NEO_TNFS_MOUNT ? 0x34 : req[0];
    r[1] = req[3] == NEO_TNFS_MOUNT ? 0x12 : req[1];
    r[2] = req[2];
    r[3] = req[3];
    r[4] = 0;
    if (req[3] == NEO_TNFS_MOUNT) {
        r[5] = 2; r[6] = 1; r[7] = 0; r[8] = 0;
        len = 9;
    } else if (req[3] == NEO_TNFS_STAT) {
        r[5] = 0xA4; r[6] = 0x81;
        r[11] = 0x39; r[12] = 0x05;   // Size 1337
        len = 5 + 20;
    } else {
        r[4] = 0x16;   // ENOSYS
    }
    neo_netbridge_udp_in(&bridge, r, len);   // The answer comes back at once
    return true;
}

static char cfg_ssid[33], cfg_pass[64], cfg_server[64];
static uint16_t cfg_port;
static void bridge_configure(void* ctx, const char* ssid, const char* pass, const char* server, uint16_t port) {
    (void)ctx;
    snprintf(cfg_ssid, sizeof(cfg_ssid), "%s", ssid);
    snprintf(cfg_pass, sizeof(cfg_pass), "%s", pass);
    snprintf(cfg_server, sizeof(cfg_server), "%s", server);
    cfg_port = port;
}
static uint8_t bridge_status(void* ctx, uint8_t* ip, uint8_t* server_ip) {
    (void)ctx;
    const uint8_t a[4] = {192, 168, 1, 50}, s[4] = {192, 168, 1, 10};
    memcpy(ip, a, 4);
    memcpy(server_ip, s, 4);
    return cfg_ssid[0] ? NEO_NET_UP : NEO_NET_IDLE;
}

// Host side: writes go to the bridge (as the Pico W reads its USB port)
static bool host_write(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    neo_netbridge_serial_in(&bridge, p, n);
    return true;
}
static int host_read(void* ctx, uint32_t timeout_us) {
    (void)ctx;
    (void)timeout_us;
    if (to_host_head == to_host_tail) return -1;
    return to_host[to_host_head++ % sizeof(to_host)];
}

static void test_chain(void) {
    bridge.serial_write = bridge_serial_write;
    bridge.udp_send = bridge_udp_send;
    bridge.configure = bridge_configure;
    bridge.status = bridge_status;
    neo_netbridge_init(&bridge);
    static neo_netlink_t link;
    neo_netlink_init(&link, host_write, host_read, 0);
    // Status, configuration, status again
    CHECK(neo_netlink_status(&link, 0, 0, 100) == NEO_NET_IDLE);
    CHECK(neo_netlink_configure(&link, "MaBox", "mot de passe", "192.168.1.10", 16384));
    CHECK(!strcmp(cfg_ssid, "MaBox") && !strcmp(cfg_pass, "mot de passe") && !strcmp(cfg_server, "192.168.1.10") &&
          cfg_port == 16384);
    uint8_t ip[4], sip[4];
    CHECK(neo_netlink_status(&link, ip, sip, 100) == NEO_NET_UP && ip[3] == 50 && sip[3] == 10);
    // A log frame from the bridge is skipped by the client
    neo_netbridge_log(&bridge, "wifi: connected");
    // TNFS over the link
    static neo_tnfs_t t;
    neo_tnfs_init(&t, neo_netlink_xfer, &link);
    CHECK(neo_tnfs_mount(&t, "/", 0, 0) && t.session == 0x1234 && t.version == 0x0102);
    uint32_t size = 0;
    CHECK(neo_tnfs_stat(&t, "/X.DSK", &size, 0) && size == 1337);
    CHECK(link.logs == 1 && bridge.requests == 2 && bridge.replies == 2 && udp_sent == 2);
    CHECK(neo_tnfs_open(&t, "/X.DSK", NEO_TNFS_O_RDONLY, 0) < 0 && t.error == 0x16);   // ENOSYS from the fake server
    CHECK(link.dec.bad == 0 && bridge.dec.bad == 0);
}

int main(void) {
    test_frames();
    test_chain();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
