// test_neo_tnfs.c — client TNFS (src/devices/neo_tnfs.h) contre un faux serveur
//
// Le faux serveur, en mémoire, suit la spécification TNFS (spectranet,
// tnfs-protocol.md) pour les commandes obligatoires et le comportement de tnfsd
// (même numéro de séquence : la dernière réponse est renvoyée). Il sait perdre
// une requête ou une réponse, glisser une réponse périmée et répondre EAGAIN,
// pour éprouver les répétitions du client. Le serveur réel (tnfsd) est essayé
// par tests/oric/run_integration.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#define NEO_TNFS_IMPL
#include "devices/neo_tnfs.h"
#include "devices/oric_tape.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

/*-- Fake server -------------------------------------------------------------*/
#define SRV_FILES 6
static struct {
    char name[32];
    uint8_t data[4096];
    uint32_t size;
    bool exists;
} srv_file[SRV_FILES];
static struct {
    int file;   // Index in srv_file, -1: free
    uint32_t pos;
} srv_fd[8];
static int srv_dir_pos = -1;
static uint16_t srv_session = 0xBEEF;
static uint8_t srv_last_seq = 0xFF, srv_last[NEO_TNFS_MSG_MAX];
static int srv_last_len;
static bool srv_mounted;
// Faults: lose the next request / the next answer, send a stale answer first, answer EAGAIN once
static int drop_request, drop_answer, stale_answer, eagain_once;
static int srv_requests;

static int srv_find(const char* path) {
    while (*path == '/') path++;
    for (int i = 0; i < SRV_FILES; i++) {
        if (srv_file[i].exists && !strcmp(srv_file[i].name, path)) return i;
    }
    return -1;
}

static int srv_reply(const uint8_t* req, uint8_t status, const uint8_t* data, int len, uint8_t* resp) {
    resp[0] = req[3] == NEO_TNFS_MOUNT ? (uint8_t)srv_session : req[0];
    resp[1] = req[3] == NEO_TNFS_MOUNT ? (uint8_t)(srv_session >> 8) : req[1];
    resp[2] = req[2];
    resp[3] = req[3];
    resp[4] = status;
    if (len) memcpy(resp + 5, data, (size_t)len);
    return 5 + len;
}

static int srv_handle(const uint8_t* req, int len, uint8_t* resp) {
    const uint8_t cmd = req[3];
    const uint8_t* p = req + 4;
    uint8_t out[NEO_TNFS_MSG_MAX];
    if (cmd != NEO_TNFS_MOUNT && (req[0] | (req[1] << 8)) != srv_session) return srv_reply(req, 0xFF, 0, 0, resp);
    switch (cmd) {
        case NEO_TNFS_MOUNT:
            srv_mounted = true;
            out[0] = 0x02; out[1] = 0x01; out[2] = 0x88; out[3] = 0x13;   // Version 1.2, retry 5000 ms
            return srv_reply(req, 0, out, 4, resp);
        case NEO_TNFS_UMOUNT:
            srv_mounted = false;
            return srv_reply(req, 0, 0, 0, resp);
        case NEO_TNFS_STAT: {
            const int f = srv_find((const char*)p);
            if (f < 0) return srv_reply(req, NEO_TNFS_ENOENT, 0, 0, resp);
            memset(out, 0, 22);
            out[0] = 0xA4; out[1] = 0x81;   // 0100644: regular file
            for (int i = 0; i < 4; i++) out[6 + i] = (uint8_t)(srv_file[f].size >> (8 * i));
            out[18] = 0; out[19] = 0;       // Empty uid / gid strings
            return srv_reply(req, 0, out, 20, resp);
        }
        case NEO_TNFS_OPEN: {
            const uint16_t flags = (uint16_t)(p[0] | (p[1] << 8));
            const char* name = (const char*)p + 4;
            int f = srv_find(name);
            if (f < 0 && (flags & NEO_TNFS_O_CREAT)) {
                for (f = 0; f < SRV_FILES && srv_file[f].exists; f++) {}
                if (f == SRV_FILES) return srv_reply(req, 0x12, 0, 0, resp);   // ENOSPC
                while (*name == '/') name++;
                snprintf(srv_file[f].name, sizeof(srv_file[f].name), "%s", name);
                srv_file[f].exists = true;
                srv_file[f].size = 0;
            }
            if (f < 0) return srv_reply(req, NEO_TNFS_ENOENT, 0, 0, resp);
            if (flags & NEO_TNFS_O_TRUNC) srv_file[f].size = 0;
            for (int d = 0; d < 8; d++) {
                if (srv_fd[d].file < 0) {
                    srv_fd[d].file = f;
                    srv_fd[d].pos = 0;
                    out[0] = (uint8_t)d;
                    return srv_reply(req, 0, out, 1, resp);
                }
            }
            return srv_reply(req, 0x10, 0, 0, resp);   // EMFILE
        }
        case NEO_TNFS_CLOSE:
            if (p[0] >= 8 || srv_fd[p[0]].file < 0) return srv_reply(req, 0x06, 0, 0, resp);
            srv_fd[p[0]].file = -1;
            return srv_reply(req, 0, 0, 0, resp);
        case NEO_TNFS_LSEEK: {
            if (p[0] >= 8 || srv_fd[p[0]].file < 0) return srv_reply(req, 0x06, 0, 0, resp);
            srv_fd[p[0]].pos = (uint32_t)(p[2] | (p[3] << 8) | (p[4] << 16) | ((uint32_t)p[5] << 24));
            for (int i = 0; i < 4; i++) out[i] = (uint8_t)(srv_fd[p[0]].pos >> (8 * i));
            return srv_reply(req, 0, out, 4, resp);
        }
        case NEO_TNFS_READ: {
            if (p[0] >= 8 || srv_fd[p[0]].file < 0) return srv_reply(req, 0x06, 0, 0, resp);
            const int f = srv_fd[p[0]].file;
            uint32_t want = (uint32_t)(p[1] | (p[2] << 8));
            if (want > NEO_TNFS_IO_MAX) want = NEO_TNFS_IO_MAX;
            const uint32_t pos = srv_fd[p[0]].pos;
            if (pos >= srv_file[f].size) return srv_reply(req, NEO_TNFS_EOF, 0, 0, resp);
            uint32_t n = srv_file[f].size - pos < want ? srv_file[f].size - pos : want;
            if (n > 200) n = 200;   // Short reads: the client must loop
            out[0] = (uint8_t)n;
            out[1] = (uint8_t)(n >> 8);
            memcpy(out + 2, srv_file[f].data + pos, n);
            srv_fd[p[0]].pos += n;
            return srv_reply(req, 0, out, (int)n + 2, resp);
        }
        case NEO_TNFS_WRITE: {
            if (p[0] >= 8 || srv_fd[p[0]].file < 0) return srv_reply(req, 0x06, 0, 0, resp);
            const int f = srv_fd[p[0]].file;
            const uint32_t n = (uint32_t)(p[1] | (p[2] << 8)), pos = srv_fd[p[0]].pos;
            if (pos + n > sizeof(srv_file[f].data) || (int)n + 7 > len) return srv_reply(req, 0x12, 0, 0, resp);
            memcpy(srv_file[f].data + pos, p + 3, n);
            srv_fd[p[0]].pos += n;
            if (srv_fd[p[0]].pos > srv_file[f].size) srv_file[f].size = srv_fd[p[0]].pos;
            out[0] = (uint8_t)n;
            out[1] = (uint8_t)(n >> 8);
            return srv_reply(req, 0, out, 2, resp);
        }
        case NEO_TNFS_OPENDIR:
            if (strcmp((const char*)p, "/")) return srv_reply(req, NEO_TNFS_ENOENT, 0, 0, resp);
            srv_dir_pos = -2;   // ".", "..", then the files
            out[0] = 3;
            return srv_reply(req, 0, out, 1, resp);
        case NEO_TNFS_READDIR: {
            const char* name = 0;
            if (srv_dir_pos == -2) name = ".";
            else if (srv_dir_pos == -1) name = "..";
            else {
                while (srv_dir_pos < SRV_FILES && !srv_file[srv_dir_pos].exists) srv_dir_pos++;
                if (srv_dir_pos >= SRV_FILES) return srv_reply(req, NEO_TNFS_EOF, 0, 0, resp);
                name = srv_file[srv_dir_pos].name;
            }
            srv_dir_pos++;
            return srv_reply(req, 0, (const uint8_t*)name, (int)strlen(name) + 1, resp);
        }
        case NEO_TNFS_UNLINK: {
            const int f = srv_find((const char*)p);
            if (f < 0) return srv_reply(req, NEO_TNFS_ENOENT, 0, 0, resp);
            srv_file[f].exists = false;
            return srv_reply(req, 0, 0, 0, resp);
        }
        case NEO_TNFS_CLOSEDIR:
            srv_dir_pos = -1;
            return srv_reply(req, 0, 0, 0, resp);
        default:
            return srv_reply(req, 0x16, 0, 0, resp);   // ENOSYS
    }
}

static int fake_xfer(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms) {
    (void)ctx;
    (void)max;
    (void)timeout_ms;
    srv_requests++;
    if (drop_request) {
        drop_request--;
        return -1;
    }
    if (stale_answer) {
        // An answer to an older request arrives first: wrong sequence number
        stale_answer--;
        uint8_t old[5] = {req[0], req[1], (uint8_t)(req[2] - 1), req[3], 0};
        memcpy(resp, old, 5);
        return 5;
    }
    if (req[2] == srv_last_seq && req[3] != NEO_TNFS_MOUNT) {
        memcpy(resp, srv_last, (size_t)srv_last_len);   // Resend of the last answer, as tnfsd
        return srv_last_len;
    }
    int n;
    if (eagain_once && req[3] == NEO_TNFS_READ) {
        eagain_once = 0;
        const uint8_t back[2] = {10, 0};
        n = srv_reply(req, NEO_TNFS_EAGAIN, back, 2, resp);
    } else {
        n = srv_handle(req, len, resp);
    }
    srv_last_seq = req[2];
    memcpy(srv_last, resp, (size_t)n);
    srv_last_len = n;
    if (drop_answer) {
        drop_answer--;
        return -1;   // Done by the server, answer lost: the resend must not do it twice
    }
    return n;
}

static uint32_t slept;
static void fake_sleep(uint32_t ms) { slept += ms; }

static void srv_reset(void) {
    memset(srv_file, 0, sizeof(srv_file));
    for (int d = 0; d < 8; d++) srv_fd[d].file = -1;
    srv_last_seq = 0xFF;
    drop_request = drop_answer = stale_answer = eagain_once = 0;
}

/*-- Tests -------------------------------------------------------------------*/
static neo_tnfs_t t;

static void test_client(void) {
    srv_reset();
    neo_tnfs_init(&t, fake_xfer, 0);
    t.sleep = fake_sleep;
    CHECK(neo_tnfs_mount(&t, "/", 0, 0) && t.mounted && t.session == 0xBEEF && t.version == 0x0102);
    // Create and write 1000 bytes (two WRITE), read them back in short READs
    int fd = neo_tnfs_open(&t, "/A.DSK", NEO_TNFS_O_RDWR | NEO_TNFS_O_CREAT | NEO_TNFS_O_TRUNC, 0644);
    CHECK(fd >= 0);
    uint8_t data[1000], back[1000];
    for (int i = 0; i < 1000; i++) data[i] = (uint8_t)(i * 13 + 1);
    CHECK(neo_tnfs_write(&t, (uint8_t)fd, data, sizeof(data)));
    CHECK(neo_tnfs_seek(&t, (uint8_t)fd, 0) && neo_tnfs_read(&t, (uint8_t)fd, back, sizeof(back)));
    CHECK(!memcmp(data, back, sizeof(data)));
    CHECK(!neo_tnfs_read(&t, (uint8_t)fd, back, 1) && t.error == NEO_TNFS_EOF);
    uint32_t size = 0;
    uint16_t mode = 0;
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, &mode) && size == 1000 && (mode & NEO_TNFS_S_IFMT) == 0100000);
    CHECK(!neo_tnfs_stat(&t, "/NONE", &size, &mode) && t.error == NEO_TNFS_ENOENT);
    // Lost request, lost answer (the WRITE must be done once), stale answer, EAGAIN
    const uint32_t resent = t.resent;
    drop_request = 1;
    CHECK(neo_tnfs_seek(&t, (uint8_t)fd, 10));
    drop_answer = 1;
    CHECK(neo_tnfs_write(&t, (uint8_t)fd, (const uint8_t*)"XY", 2));
    CHECK(neo_tnfs_stat(&t, "/A.DSK", &size, 0) && size == 1000);   // Not written twice at 12
    stale_answer = 1;
    CHECK(neo_tnfs_seek(&t, (uint8_t)fd, 9));
    eagain_once = 1;
    CHECK(neo_tnfs_read(&t, (uint8_t)fd, back, 4) && back[0] == data[9] && back[1] == 'X' && back[2] == 'Y');
    CHECK(slept == 10 && t.resent == resent + 3);
    CHECK(neo_tnfs_close(&t, (uint8_t)fd));
    // No answer at all: failure after t.tries attempts
    drop_request = 100;
    const int before = srv_requests;
    CHECK(!neo_tnfs_stat(&t, "/A.DSK", &size, 0) && t.error == 0xFF && srv_requests - before == t.tries);
    drop_request = 0;
    CHECK(neo_tnfs_umount(&t) && !t.mounted);
}

static int listed;
static bool list_cb(const neo_dirent_t* e, void* user) {
    (void)user;
    if (!strcmp(e->name, "B.TAP")) CHECK(e->size == 17 && !e->dir);
    listed++;
    return true;
}

static void test_volume(void) {
    srv_reset();
    neo_tnfs_init(&t, fake_xfer, 0);
    neo_storage_set(1, "Réseau", &neo_storage_tnfs_ops, &t);
    CHECK(!neo_storage_ready(1));   // Not mounted yet
    CHECK(neo_tnfs_mount(&t, "/", 0, 0) && neo_storage_ready(1));
    // A .tap saved on the server, read by the Oric tape reader as a stream
    static const uint8_t tap[17] = {0x16, 0x16, 0x16, 0x24, 0, 0, 0x80, 0, 0x05, 0x01, 0x05, 0x00, 0, 'A', 0, 0x42, 0x43};
    CHECK(neo_file_save(1, "B.TAP", tap, sizeof(tap)));
    neo_file_t f;
    CHECK(neo_file_open(&f, 1, "/B.TAP", NEO_READ) && f.size == 17);
    static oric_tape_t ot;
    memset(&ot, 0, sizeof(ot));
    oric_tape_insert(&ot, f.size, neo_file_read_cb, &f);
    CHECK(ot.inserted && ot.len == 17);
    uint8_t b[4];
    CHECK(neo_file_read(&f, 13, b, 4) && b[0] == 'A' && b[3] == 0x43);
    CHECK(neo_file_read(&f, 0, b, 2) && b[0] == 0x16);   // Backwards: LSEEK again
    CHECK(neo_file_close(&f));
    // Random access writing, as the WD1793 does on a streamed disk image
    static uint8_t img[3000];
    for (int i = 0; i < 3000; i++) img[i] = (uint8_t)i;
    CHECK(neo_file_save(1, "C.DSK", img, 3000));
    CHECK(neo_file_open(&f, 1, "C.DSK", NEO_READ | NEO_WRITE) && f.size == 3000);
    CHECK(neo_file_write_cb(&f, 2000, (const uint8_t*)"Z", 1));
    CHECK(neo_file_read_cb(&f, 1999, b, 3) && b[0] == img[1999] && b[1] == 'Z' && b[2] == img[2001]);
    neo_file_close(&f);
    CHECK(!neo_file_open(&f, 1, "NONE.DSK", NEO_READ));
    listed = 0;
    CHECK(neo_storage_list(1, "/", list_cb, 0) && listed == 2);
    CHECK(neo_file_remove(1, "C.DSK") && !neo_file_open(&f, 1, "C.DSK", NEO_READ));
    CHECK(!neo_file_remove(1, "C.DSK") && t.error == NEO_TNFS_ENOENT);
    neo_tnfs_umount(&t);
    CHECK(!neo_storage_ready(1));
}

int main(void) {
    test_client();
    test_volume();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
