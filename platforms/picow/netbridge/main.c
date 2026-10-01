// main.c — passerelle réseau Pico W de la Neo6502 (neo_netbridge)
//
// La Neo6502 voit le Pico W comme un port série USB (sur son hub) ; les trames
// neo_netlink (src/devices/neo_netlink.h) y portent des datagrammes TNFS, que le
// Pico W envoie en UDP au serveur par le Wi-Fi, et dont il renvoie les réponses.
// Même cœur que la passerelle PC des tests (platforms/pc/netbridge).
//
// Réseau Wi-Fi et serveur : trame NEO_NET_CONFIG envoyée par la Neo6502 (ou
// oric_headless -S /dev/ttyACM0 -N SERVEUR avec NEO_WIFI_SSID / NEO_WIFI_PASS),
// ou valeurs par défaut à la compilation (-DWIFI_SSID=… -DWIFI_PASSWORD=…
// -DTNFS_SERVER=…). La DEL du Pico W s'allume quand le Wi-Fi est connecté.
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define NEO_NETLINK_IMPL
#include "devices/neo_netlink.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef TNFS_SERVER
#define TNFS_SERVER ""
#endif

static neo_netbridge_t bridge;
static struct udp_pcb *pcb;
static ip_addr_t server_ip;
static uint16_t server_port = 16384;
static bool server_set;
static char ssid[33], password[64];
static bool joining;

static void serial_write(void *ctx, const uint8_t *p, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) putchar_raw(p[i]);
    stdio_flush();
}

static bool link_up(void) { return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP; }

static bool udp_send_cb(void *ctx, const uint8_t *p, int n) {
    (void)ctx;
    if (!server_set || !link_up()) return false;
    struct pbuf *b = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (!b) return false;
    memcpy(b->payload, p, (size_t)n);
    const err_t e = udp_sendto(pcb, b, &server_ip, server_port);
    pbuf_free(b);
    return e == ERR_OK;
}

static void udp_recv_cb(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    (void)upcb;
    (void)addr;
    (void)port;
    if (!p) return;
    static uint8_t buf[NEO_NET_FRAME_MAX];
    const u16_t n = pbuf_copy_partial(p, buf, p->tot_len < sizeof(buf) ? p->tot_len : sizeof(buf), 0);
    pbuf_free(p);
    neo_netbridge_udp_in(&bridge, buf, n);
}

static void join(void) {
    if (!ssid[0]) return;
    cyw43_arch_wifi_connect_async(ssid, password, password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    joining = true;
}

static void configure_cb(void *ctx, const char *s, const char *pw, const char *server, uint16_t port) {
    (void)ctx;
    if (server[0]) {
        server_set = ipaddr_aton(server, &server_ip) != 0;   // An IP address (no DNS in this version)
        server_port = port ? port : 16384;
        neo_netbridge_log(&bridge, server_set ? "netbridge: server set" : "netbridge: server must be an IP address");
    }
    if (s[0] && (strcmp(s, ssid) || strcmp(pw, password) || !link_up())) {
        snprintf(ssid, sizeof(ssid), "%s", s);
        snprintf(password, sizeof(password), "%s", pw);
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        join();
        neo_netbridge_log(&bridge, "netbridge: joining the Wi-Fi network");
    }
}

static uint8_t status_cb(void *ctx, uint8_t *ip, uint8_t *sip) {
    (void)ctx;
    const int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    const ip4_addr_t *a = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    memcpy(ip, &a->addr, 4);
    memcpy(sip, &ip_2_ip4(&server_ip)->addr, 4);
    if (st == CYW43_LINK_UP) return server_set ? NEO_NET_UP : NEO_NET_CONNECTING;
    if (st < 0) return NEO_NET_FAILED;
    return joining ? NEO_NET_CONNECTING : NEO_NET_IDLE;
}

int main(void) {
    stdio_init_all();   // USB serial only (pico_enable_stdio_usb): the link to the Neo6502
    if (cyw43_arch_init()) return 1;
    cyw43_arch_enable_sta_mode();
    pcb = udp_new();
    udp_bind(pcb, IP_ADDR_ANY, 0);
    udp_recv(pcb, udp_recv_cb, 0);

    bridge.serial_write = serial_write;
    bridge.udp_send = udp_send_cb;
    bridge.configure = configure_cb;
    bridge.status = status_cb;
    neo_netbridge_init(&bridge);
    if (TNFS_SERVER[0]) configure_cb(0, WIFI_SSID, WIFI_PASSWORD, TNFS_SERVER, 16384);

    bool led = false;
    for (;;) {
        cyw43_arch_poll();
        // Bytes from the Neo6502
        for (int k = 0; k < 64; k++) {
            const int c = getchar_timeout_us(0);
            if (c == PICO_ERROR_TIMEOUT) break;
            const uint8_t b = (uint8_t)c;
            neo_netbridge_serial_in(&bridge, &b, 1);
        }
        const bool up = link_up();
        if (up != led) {
            led = up;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, up);
            if (up) joining = false;
        }
    }
}
