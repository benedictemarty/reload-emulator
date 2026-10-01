#pragma once

// neo_tnfs_udp.h — transport UDP du client TNFS (src/devices/neo_tnfs.h) sur PC
//
//   neo_tnfs_udp_t u;
//   if (neo_tnfs_udp_open(&u, "127.0.0.1", NEO_TNFS_PORT)) neo_tnfs_init(&t, neo_tnfs_udp_xfer, &u);
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "devices/neo_tnfs.h"

typedef struct {
    int fd;
    struct sockaddr_in server;
} neo_tnfs_udp_t;

static bool neo_tnfs_udp_open(neo_tnfs_udp_t* u, const char* host, uint16_t port) {
    memset(u, 0, sizeof(*u));
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, 0, &hints, &res) != 0 || !res) return false;
    memcpy(&u->server, res->ai_addr, sizeof(u->server));
    u->server.sin_port = htons(port);
    freeaddrinfo(res);
    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    return u->fd >= 0;
}

static void neo_tnfs_udp_close(neo_tnfs_udp_t* u) {
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
}

// One datagram out, the first datagram from the server back (or -1 after timeout_ms)
static int neo_tnfs_udp_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms) {
    neo_tnfs_udp_t* u = (neo_tnfs_udp_t*)ctx;
    if (sendto(u->fd, req, (size_t)len, 0, (const struct sockaddr*)&u->server, sizeof(u->server)) != len) return -1;
    struct pollfd p = {u->fd, POLLIN, 0};
    if (poll(&p, 1, (int)timeout_ms) <= 0) return -1;
    const ssize_t n = recv(u->fd, resp, (size_t)max, 0);
    return n < 0 ? -1 : (int)n;
}

static void neo_tnfs_udp_sleep(uint32_t ms) { usleep(ms * 1000u); }
