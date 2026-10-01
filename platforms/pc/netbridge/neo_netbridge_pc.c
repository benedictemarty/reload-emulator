// neo_netbridge_pc.c — passerelle réseau sur PC, jumelle du firmware Pico W
//
// Même rôle que platforms/picow/netbridge : trames neo_netlink sur une liaison
// série d'un côté, datagrammes UDP vers un serveur TNFS de l'autre (cœur commun :
// neo_netbridge_* de src/devices/neo_netlink.h). La liaison est un
// pseudo-terminal créé ici : oric_headless -S <son chemin> y parle comme à un Pico W.
//
//   neo_netbridge_pc [-s SERVEUR[:PORT]] [-p FICHIER]
//     -s  serveur TNFS (défaut 127.0.0.1:16384 ; une trame CONFIG peut le changer)
//     -p  écrit le chemin du pseudo-terminal dans FICHIER (pour les scripts)
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#define _DEFAULT_SOURCE   // posix_openpt, ptsname and cfmakeraw
#define _XOPEN_SOURCE 600
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define NEO_NETLINK_IMPL
#include "devices/neo_netlink.h"

static int pty_fd = -1, udp_fd = -1;
static struct sockaddr_in server;
static volatile sig_atomic_t stop;

static bool set_server(const char* host, uint16_t port) {
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, 0, &hints, &res) != 0 || !res) return false;
    memcpy(&server, res->ai_addr, sizeof(server));
    server.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

static void serial_write(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    while (n > 0) {
        const ssize_t w = write(pty_fd, p, (size_t)n);
        if (w <= 0) return;
        p += w;
        n -= (int)w;
    }
}

static bool udp_send(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    return sendto(udp_fd, p, (size_t)n, 0, (const struct sockaddr*)&server, sizeof(server)) == n;
}

static void configure(void* ctx, const char* ssid, const char* password, const char* host, uint16_t port) {
    (void)ctx;
    (void)password;
    // No Wi-Fi here: only the server is taken from the configuration
    if (host[0] && set_server(host, port ? port : 16384)) {
        fprintf(stderr, "netbridge: server %s:%u (ssid \"%s\" ignored on the PC)\n", host, port, ssid);
    }
}

static uint8_t status(void* ctx, uint8_t* ip, uint8_t* server_ip) {
    (void)ctx;
    const uint8_t local[4] = {127, 0, 0, 1};
    memcpy(ip, local, 4);
    memcpy(server_ip, &server.sin_addr.s_addr, 4);
    return NEO_NET_UP;
}

static void on_signal(int s) {
    (void)s;
    stop = 1;
}

int main(int argc, char** argv) {
    const char* server_arg = "127.0.0.1:16384";
    const char* path_file = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) server_arg = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) path_file = argv[++i];
        else {
            fprintf(stderr, "usage: neo_netbridge_pc [-s SERVER[:PORT]] [-p FILE]\n");
            return 2;
        }
    }
    char host[256];
    snprintf(host, sizeof(host), "%s", server_arg);
    char* colon = strchr(host, ':');
    const uint16_t port = colon ? (uint16_t)atoi(colon + 1) : 16384;
    if (colon) *colon = 0;
    if (!set_server(host, port)) {
        fprintf(stderr, "netbridge: unknown server %s\n", host);
        return 2;
    }
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    pty_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (udp_fd < 0 || pty_fd < 0 || grantpt(pty_fd) || unlockpt(pty_fd)) {
        perror("netbridge");
        return 1;
    }
    struct termios tio;
    if (tcgetattr(pty_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(pty_fd, TCSANOW, &tio);
    }
    const char* name = ptsname(pty_fd);
    printf("%s\n", name);
    fflush(stdout);
    if (path_file) {
        FILE* f = fopen(path_file, "w");
        if (f) {
            fprintf(f, "%s\n", name);
            fclose(f);
        }
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    static neo_netbridge_t b;
    b.serial_write = serial_write;
    b.udp_send = udp_send;
    b.configure = configure;
    b.status = status;
    neo_netbridge_init(&b);
    while (!stop) {
        struct pollfd p[2] = {{pty_fd, POLLIN, 0}, {udp_fd, POLLIN, 0}};
        if (poll(p, 2, 200) <= 0) continue;
        uint8_t buf[NEO_NET_FRAME_MAX];
        if (p[0].revents & POLLIN) {
            const ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) neo_netbridge_serial_in(&b, buf, (int)n);
        }
        if (p[1].revents & POLLIN) {
            const ssize_t n = recv(udp_fd, buf, sizeof(buf), 0);
            if (n > 0) neo_netbridge_udp_in(&b, buf, (int)n);
        }
    }
    fprintf(stderr, "netbridge: %u request(s), %u reply(ies), %u dropped, %u bad frame(s)\n", (unsigned)b.requests,
            (unsigned)b.replies, (unsigned)b.dropped, (unsigned)b.dec.bad);
    return 0;
}
