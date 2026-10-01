// test_neo_dgram.c — transport TNFS « un datagramme par trame » (src/devices/neo_dgram_serial.h)
//
// Relais simulé : trames longueur (2 octets, petit-boutiste) + datagramme vers un
// faux serveur TNFS (MOUNT, STAT) ; réponse en retard, longueur fausse, trame tronquée.

#include <stdio.h>
#include <string.h>

#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#define NEO_TNFS_IMPL
#include "devices/neo_tnfs.h"
#define NEO_DGRAM_SERIAL_IMPL
#include "devices/neo_dgram_serial.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static uint8_t to_host[16384];
static int head, tail;
static void push(const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) to_host[tail++ % sizeof(to_host)] = p[i];
}

// Relay: decodes the frames written by the host, answers through a fake server
static uint8_t in[2048];
static int in_len;
static bool drop_answer, late_answer, bad_length;
static uint8_t late[64];
static int late_len;

static int server(const uint8_t* req, uint8_t* r) {
    memset(r, 0, 32);
    r[0] = req[3] == NEO_TNFS_MOUNT ? 0x21 : req[0];
    r[1] = req[3] == NEO_TNFS_MOUNT ? 0x43 : req[1];
    r[2] = req[2];
    r[3] = req[3];
    if (req[3] == NEO_TNFS_MOUNT) {
        r[5] = 2; r[6] = 1;
        return 9;
    }
    r[5] = 0xA4; r[6] = 0x81; r[11] = 0x00; r[12] = 0x02;   // STAT: size 512
    return 25;
}

static void frame_out(const uint8_t* p, int n) {
    const uint8_t h[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
    push(h, 2);
    push(p, n);
}

static bool host_write(void* ctx, const uint8_t* p, int n) {
    (void)ctx;
    memcpy(in + in_len, p, (size_t)n);
    in_len += n;
    while (in_len >= 2) {
        const int len = in[0] | (in[1] << 8);
        if (in_len < 2 + len) break;
        uint8_t r[64];
        const int rn = server(in + 2, r);
        if (late_len) {   // The late answer of the previous request arrives first
            frame_out(late, late_len);
            late_len = 0;
        }
        if (bad_length) {
            bad_length = false;
            const uint8_t junk[3] = {0xFF, 0xFF, 0x00};
            push(junk, 3);
        } else if (late_answer) {
            late_answer = false;
            memcpy(late, r, (size_t)rn);
            late_len = rn;   // Delivered with the next request
        } else if (!drop_answer) {
            frame_out(r, rn);
        }
        memmove(in, in + 2 + len, (size_t)(in_len - 2 - len));
        in_len -= 2 + len;
    }
    return true;
}
static int host_read(void* ctx, uint32_t timeout_us) {
    (void)ctx;
    (void)timeout_us;
    return head == tail ? -1 : to_host[head++ % sizeof(to_host)];
}

int main(void) {
    static neo_dgram_t d;
    neo_dgram_init(&d, host_write, host_read, 0);
    static neo_tnfs_t t;
    neo_tnfs_init(&t, neo_dgram_xfer, &d);
    CHECK(neo_tnfs_mount(&t, "/", 0, 0) && t.session == 0x4321 && t.version == 0x0102);
    uint32_t size = 0;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0) && size == 512 && t.resent == 0);
    // Late answer: dropped before the next request, which then gets its own
    late_answer = true;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0));   // First try times out, the resend succeeds
    const uint32_t resent = t.resent;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0) && t.resent == resent && d.stale >= 1);   // Back in step
    // Wrong length: drained, then the resend succeeds
    bad_length = true;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0) && d.resync == 1);
    // No answer at all: failure after the client's tries
    drop_answer = true;
    CHECK(!neo_tnfs_stat(&t, "/A.DSK", &size, 0) && t.error == 0xFF);
    drop_answer = false;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0));
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
