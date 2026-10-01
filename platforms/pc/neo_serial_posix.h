#pragma once

// neo_serial_posix.h — liaison série du PC (port série USB, pseudo-terminal)
// pour neo_netlink (src/devices/neo_netlink.h) : la passerelle Pico W branchée
// sur le PC (/dev/ttyACM0) ou la passerelle PC des tests (pseudo-terminal).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

// Ouvre le port en mode brut (aucune traduction de caractères) ; -1 en cas d'échec
static int neo_serial_open(const char* path) {
    const int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);   // Without effect on a USB serial port; used by a real UART
        cfsetospeed(&tio, B115200);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

static bool neo_serial_write(void* ctx, const uint8_t* p, int n) {
    const int fd = (int)(intptr_t)ctx;
    while (n > 0) {
        const ssize_t w = write(fd, p, (size_t)n);
        if (w <= 0) return false;
        p += w;
        n -= (int)w;
    }
    return true;
}

static int neo_serial_read_byte(void* ctx, uint32_t timeout_us) {
    const int fd = (int)(intptr_t)ctx;
    struct pollfd pf = {fd, POLLIN, 0};
    if (poll(&pf, 1, (int)((timeout_us + 999) / 1000)) <= 0) return -1;
    uint8_t c;
    return read(fd, &c, 1) == 1 ? c : -1;
}
