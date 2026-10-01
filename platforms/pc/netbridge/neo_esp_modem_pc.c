// neo_esp_modem_pc.c — modem Wi-Fi « AT ESP8266 » simulé sur PC (UDP réel)
//
// Jumeau de Neo6502picowifi pour les tests : un pseudo-terminal parle le dialogue
// AT convenu (platforms/pc/neo_esp_fake.h), les datagrammes partent en UDP vers
// le serveur donné par AT+CIPSTART. oric_headless -S <pseudo-terminal> -N SERVEUR
// y parle comme au Pico W branché sur /dev/ttyACM0.
//
//   neo_esp_modem_pc [-p FICHIER]   (-p : écrit le chemin des deux pseudo-terminaux,
//                                    port AT puis port TNFS, un par ligne)
//
// Le second pseudo-terminal joue le second port série USB de Neo6502picowifi
// (eeda7d3) : trames longueur + datagramme, serveur donné par AT$TNFS sur le port AT.
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

#include "../neo_esp_fake.h"

static int pty_fd = -1, udp_fd = -1, pty2_fd = -1, udp2_fd = -1;
static struct sockaddr_in server;
static volatile sig_atomic_t stop;

static void out(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    while (n > 0) {
        const ssize_t w = write(pty_fd, p, (size_t)n);
        if (w <= 0) return;
        p += w;
        n -= (int)w;
    }
}

static bool udp_open(void* ctx, const char* host, uint16_t port) {
    (void)ctx;
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, 0, &hints, &res) != 0 || !res) return false;
    memcpy(&server, res->ai_addr, sizeof(server));
    server.sin_port = htons(port);
    freeaddrinfo(res);
    if (udp_fd < 0) udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    return udp_fd >= 0;
}

static bool udp_send(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    return sendto(udp_fd, p, (size_t)n, 0, (const struct sockaddr*)&server, sizeof(server)) == n;
}

static void out2(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    while (n > 0) {
        const ssize_t w = write(pty2_fd, p, (size_t)n);
        if (w <= 0) return;
        p += w;
        n -= (int)w;
    }
}

// TNFS relay: its own UDP socket, connected to the server of AT$TNFS (only its answers come back)
static char relay_host[128];
static uint16_t relay_port;
static bool tnfs_send(void* ctx, const char* host, uint16_t port, const uint8_t* p, int n) {
    (void)ctx;
    if (udp2_fd < 0 || strcmp(host, relay_host) || port != relay_port) {
        if (udp2_fd >= 0) close(udp2_fd);
        struct addrinfo hints, *res = 0;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, 0, &hints, &res) != 0 || !res) return false;
        struct sockaddr_in a;
        memcpy(&a, res->ai_addr, sizeof(a));
        a.sin_port = htons(port);
        freeaddrinfo(res);
        udp2_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp2_fd < 0 || connect(udp2_fd, (struct sockaddr*)&a, sizeof(a))) return false;
        snprintf(relay_host, sizeof(relay_host), "%s", host);
        relay_port = port;
    }
    return send(udp2_fd, p, (size_t)n, 0) == n;
}

static void on_signal(int s) {
    (void)s;
    stop = 1;
}

int main(int argc, char** argv) {
    const char* path_file = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) path_file = argv[++i];
        else {
            fprintf(stderr, "usage: neo_esp_modem_pc [-p FILE]\n");
            return 2;
        }
    }
    pty_fd = posix_openpt(O_RDWR | O_NOCTTY);
    pty2_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty_fd < 0 || grantpt(pty_fd) || unlockpt(pty_fd) || pty2_fd < 0 || grantpt(pty2_fd) || unlockpt(pty2_fd)) {
        perror("esp_modem");
        return 1;
    }
    struct termios tio;
    for (int k = 0; k < 2; k++) {
        const int fd = k ? pty2_fd : pty_fd;
        if (tcgetattr(fd, &tio) == 0) {
            cfmakeraw(&tio);
            tcsetattr(fd, TCSANOW, &tio);
        }
    }
    char name[64], name2[64];
    snprintf(name, sizeof(name), "%s", ptsname(pty_fd));
    snprintf(name2, sizeof(name2), "%s", ptsname(pty2_fd));
    printf("%s\n%s\n", name, name2);
    fflush(stdout);
    if (path_file) {
        FILE* f = fopen(path_file, "w");
        if (f) {
            fprintf(f, "%s\n%s\n", name, name2);
            fclose(f);
        }
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    static neo_esp_fake_t m;
    m.out = out;
    m.udp_open = udp_open;
    m.udp_send = udp_send;
    neo_esp_fake_init(&m);
    m.tnfs_out = out2;
    m.tnfs_send = tnfs_send;
    while (!stop) {
        struct pollfd p[4] = {{pty_fd, POLLIN, 0}, {pty2_fd, POLLIN, 0}, {udp_fd, POLLIN, 0}, {udp2_fd, POLLIN, 0}};
        if (poll(p, 4, 200) <= 0) continue;
        uint8_t buf[2048];
        if (p[1].revents & POLLIN) {
            const ssize_t n = read(pty2_fd, buf, sizeof(buf));
            if (n > 0) neo_esp_fake_tnfs_input(&m, buf, (int)n);
        }
        if (udp2_fd >= 0 && (p[3].revents & POLLIN)) {
            const ssize_t n = recv(udp2_fd, buf, sizeof(buf), 0);
            if (n > 0) neo_esp_fake_tnfs_udp_in(&m, buf, (int)n);
        }
        if (p[0].revents & POLLIN) {
            const ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) neo_esp_fake_input(&m, buf, (int)n);
        }
        if (udp_fd >= 0 && (p[2].revents & POLLIN)) {
            const ssize_t n = recv(udp_fd, buf, sizeof(buf), 0);
            if (n > 0) neo_esp_fake_udp_in(&m, buf, (int)n);
        }
    }
    fprintf(stderr, "esp_modem: %u command(s), %u datagram(s) sent, %u received\n", (unsigned)m.commands,
            (unsigned)m.sent, (unsigned)m.received);
    return 0;
}
