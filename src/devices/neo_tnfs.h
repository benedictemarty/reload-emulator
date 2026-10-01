#pragma once

// neo_tnfs.h — client TNFS (Trivial Network File System) et volume neo_storage
//
// TNFS est le protocole de fichiers réseau des 8 bits (Spectranet, FujiNet) :
// spécification et serveur de référence tnfsd (licence MIT) dans
// https://github.com/FujiNetWIFI/spectranet/tree/master/tnfs. Ce client ne
// suit que les commandes obligatoires : MOUNT, UMOUNT, OPENDIR, READDIR,
// CLOSEDIR, OPEN (0x29), READ, WRITE, CLOSE, STAT, LSEEK.
//
// Le transport est fourni par l'appelant (neo_tnfs_xfer_t) : UDP sur PC
// (platforms/pc/neo_tnfs_udp.h), série USB, Wi-Fi ou Ethernet sur la carte.
// Une requête attend une seule réponse ; sans réponse dans le délai, elle est
// renvoyée avec le même numéro de séquence (le serveur renvoie alors sa
// dernière réponse) ; chaque nouvelle requête prend le numéro suivant.
//
// neo_storage_tnfs_ops sert un volume neo_storage (ctx = neo_tnfs_t monté) :
//   neo_storage_set(1, "Réseau", &neo_storage_tnfs_ops, &tnfs);
//
// C pur, sans allocation ; testé sur PC (tests/oric/test_neo_tnfs.c, et contre
// tnfsd dans tests/oric/run_integration.sh).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "devices/neo_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEO_TNFS_PORT     16384
#define NEO_TNFS_MSG_MAX  532   // Datagramme maximal (tnfsd : MAXMSGSZ)
#define NEO_TNFS_IO_MAX   512   // Données par READ / WRITE (tnfsd : MAX_IOSZ)
#ifndef NEO_TNFS_FILES
#define NEO_TNFS_FILES    8     // Descripteurs suivis par le volume
#endif

// Commandes
enum {
    NEO_TNFS_MOUNT = 0x00, NEO_TNFS_UMOUNT = 0x01,
    NEO_TNFS_OPENDIR = 0x10, NEO_TNFS_READDIR = 0x11, NEO_TNFS_CLOSEDIR = 0x12,
    NEO_TNFS_READ = 0x21, NEO_TNFS_WRITE = 0x22, NEO_TNFS_CLOSE = 0x23, NEO_TNFS_STAT = 0x24,
    NEO_TNFS_LSEEK = 0x25, NEO_TNFS_UNLINK = 0x26, NEO_TNFS_OPEN = 0x29,
};
// Drapeaux d'OPEN, codes de retour utiles
#define NEO_TNFS_O_RDONLY 0x0001
#define NEO_TNFS_O_WRONLY 0x0002
#define NEO_TNFS_O_RDWR   0x0003
#define NEO_TNFS_O_CREAT  0x0100
#define NEO_TNFS_O_TRUNC  0x0200
#define NEO_TNFS_OK       0x00
#define NEO_TNFS_ENOENT   0x02
#define NEO_TNFS_EAGAIN   0x07
#define NEO_TNFS_EOF      0x21
#define NEO_TNFS_S_IFMT   0170000
#define NEO_TNFS_S_IFDIR  0040000

// Transport : envoie req (len octets), attend une réponse au plus timeout_ms ;
// renvoie sa taille, ou -1 sans réponse
typedef int (*neo_tnfs_xfer_t)(void* ctx, const uint8_t* req, int len, uint8_t* resp, int max, uint32_t timeout_ms);
// Attente (millisecondes) pour EAGAIN ; NULL : pas d'attente
typedef void (*neo_tnfs_sleep_t)(uint32_t ms);

struct neo_tnfs;
typedef struct {
    struct neo_tnfs* owner;   // Handle of a neo_storage file: points back to its client
    bool used;
    uint8_t fd;
    uint32_t pos;             // Server position known by the client (0xFFFFFFFF: unknown)
} neo_tnfs_file_t;

typedef struct neo_tnfs {
    neo_tnfs_xfer_t xfer;
    void* xfer_ctx;
    neo_tnfs_sleep_t sleep;
    uint32_t timeout_ms;   // Par tentative (défaut 500)
    int tries;             // Tentatives par requête (défaut 5)
    uint16_t session;
    uint8_t seq;
    uint16_t version;      // Version du serveur (0x0102 = 1.2)
    bool mounted;
    uint8_t error;         // Dernier code de retour du serveur (0xFF : pas de réponse)
    uint32_t sent, resent; // Compteurs
    neo_tnfs_file_t files[NEO_TNFS_FILES];
    uint8_t req[NEO_TNFS_MSG_MAX];
    uint8_t resp[NEO_TNFS_MSG_MAX];
} neo_tnfs_t;

void neo_tnfs_init(neo_tnfs_t* t, neo_tnfs_xfer_t xfer, void* xfer_ctx);
bool neo_tnfs_mount(neo_tnfs_t* t, const char* path, const char* user, const char* password);
bool neo_tnfs_umount(neo_tnfs_t* t);
// Taille et mode POSIX d'un fichier (mode : NEO_TNFS_S_IFDIR pour un répertoire)
bool neo_tnfs_stat(neo_tnfs_t* t, const char* path, uint32_t* size, uint16_t* mode);
// Descripteur du serveur, ou -1
int neo_tnfs_open(neo_tnfs_t* t, const char* path, uint16_t flags, uint16_t mode);
bool neo_tnfs_close(neo_tnfs_t* t, uint8_t fd);
bool neo_tnfs_seek(neo_tnfs_t* t, uint8_t fd, uint32_t pos);
// Lit exactement len octets à la position courante (plusieurs READ si besoin)
bool neo_tnfs_read(neo_tnfs_t* t, uint8_t fd, uint8_t* buf, uint32_t len);
bool neo_tnfs_write(neo_tnfs_t* t, uint8_t fd, const uint8_t* buf, uint32_t len);
// Entrées du répertoire path (« . » et « .. » omis) ; cb renvoie false pour arrêter
typedef bool (*neo_tnfs_dir_cb_t)(const char* name, void* user);
bool neo_tnfs_list(neo_tnfs_t* t, const char* path, neo_tnfs_dir_cb_t cb, void* user);
// Efface un fichier (UNLINK, commande facultative du protocole : tnfsd la sert)
bool neo_tnfs_unlink(neo_tnfs_t* t, const char* path);

extern const neo_storage_ops_t neo_storage_tnfs_ops;

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef NEO_TNFS_IMPL

static void _neo_tnfs_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void _neo_tnfs_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t _neo_tnfs_get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t _neo_tnfs_get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void neo_tnfs_init(neo_tnfs_t* t, neo_tnfs_xfer_t xfer, void* xfer_ctx) {
    memset(t, 0, sizeof(*t));
    t->xfer = xfer;
    t->xfer_ctx = xfer_ctx;
    t->timeout_ms = 500;
    t->tries = 5;
}

// Envoie la requête (en-tête + plen octets déjà dans t->req + 4) et attend la
// réponse de même session, séquence et commande ; renvoie la taille des données
// après le code de retour (t->resp + 5), ou -1 (t->error : code, ou 0xFF)
static int _neo_tnfs_call(neo_tnfs_t* t, uint8_t cmd, int plen) {
    t->seq++;
    _neo_tnfs_le16(t->req, cmd == NEO_TNFS_MOUNT ? 0 : t->session);
    t->req[2] = t->seq;
    t->req[3] = cmd;
    for (int attempt = 0; attempt < t->tries; attempt++) {
        if (attempt) t->resent++;
        t->sent++;
        const int n = t->xfer(t->xfer_ctx, t->req, 4 + plen, t->resp, sizeof(t->resp), t->timeout_ms);
        if (n < 5 || t->resp[2] != t->seq || t->resp[3] != cmd) continue;   // Lost, or a late answer to another request
        if (cmd != NEO_TNFS_MOUNT && _neo_tnfs_get16(t->resp) != t->session) continue;
        t->error = t->resp[4];
        if (t->error == NEO_TNFS_EAGAIN && n >= 7) {
            // Server busy: back off as asked, then ask again (new sequence number)
            if (t->sleep) t->sleep(_neo_tnfs_get16(t->resp + 5));
            t->seq++;
            t->req[2] = t->seq;
            attempt--;   // A back-off is not a failed attempt
            continue;
        }
        return t->error == NEO_TNFS_OK ? n - 5 : -1;
    }
    t->error = 0xFF;
    return -1;
}

static int _neo_tnfs_str(uint8_t* p, const char* s) {
    const size_t n = strlen(s);
    memcpy(p, s, n + 1);
    return (int)n + 1;
}

bool neo_tnfs_mount(neo_tnfs_t* t, const char* path, const char* user, const char* password) {
    if (strlen(path) + strlen(user ? user : "") + strlen(password ? password : "") + 3 > NEO_TNFS_MSG_MAX - 6) return false;
    uint8_t* p = t->req + 4;
    _neo_tnfs_le16(p, 0x0102);   // Protocol 1.2
    int n = 2;
    n += _neo_tnfs_str(p + n, path);
    n += _neo_tnfs_str(p + n, user ? user : "");
    n += _neo_tnfs_str(p + n, password ? password : "");
    if (_neo_tnfs_call(t, NEO_TNFS_MOUNT, n) < 0) return false;
    t->session = _neo_tnfs_get16(t->resp);
    t->version = _neo_tnfs_get16(t->resp + 5);
    t->mounted = true;
    memset(t->files, 0, sizeof(t->files));
    return true;
}

bool neo_tnfs_umount(neo_tnfs_t* t) {
    if (!t->mounted) return true;
    const bool ok = _neo_tnfs_call(t, NEO_TNFS_UMOUNT, 0) >= 0;
    t->mounted = false;
    return ok;
}

bool neo_tnfs_stat(neo_tnfs_t* t, const char* path, uint32_t* size, uint16_t* mode) {
    if (strlen(path) + 1 > NEO_TNFS_MSG_MAX - 4) return false;
    const int n = _neo_tnfs_call(t, NEO_TNFS_STAT, _neo_tnfs_str(t->req + 4, path));
    if (n < 10) return false;
    if (mode) *mode = _neo_tnfs_get16(t->resp + 5);
    if (size) *size = _neo_tnfs_get32(t->resp + 11);
    return true;
}

int neo_tnfs_open(neo_tnfs_t* t, const char* path, uint16_t flags, uint16_t mode) {
    if (strlen(path) + 5 > NEO_TNFS_MSG_MAX - 4) return -1;
    _neo_tnfs_le16(t->req + 4, flags);
    _neo_tnfs_le16(t->req + 6, mode);
    const int n = _neo_tnfs_call(t, NEO_TNFS_OPEN, 4 + _neo_tnfs_str(t->req + 8, path));
    return n >= 1 ? t->resp[5] : -1;
}

bool neo_tnfs_close(neo_tnfs_t* t, uint8_t fd) {
    t->req[4] = fd;
    return _neo_tnfs_call(t, NEO_TNFS_CLOSE, 1) >= 0;
}

bool neo_tnfs_seek(neo_tnfs_t* t, uint8_t fd, uint32_t pos) {
    t->req[4] = fd;
    t->req[5] = 0;   // SEEK_SET
    _neo_tnfs_le32(t->req + 6, pos);
    return _neo_tnfs_call(t, NEO_TNFS_LSEEK, 6) >= 0;   // Servers > 1.0 also return the position
}

bool neo_tnfs_read(neo_tnfs_t* t, uint8_t fd, uint8_t* buf, uint32_t len) {
    while (len) {
        const uint16_t want = (uint16_t)(len < NEO_TNFS_IO_MAX ? len : NEO_TNFS_IO_MAX);
        t->req[4] = fd;
        _neo_tnfs_le16(t->req + 5, want);
        const int n = _neo_tnfs_call(t, NEO_TNFS_READ, 3);
        if (n < 2) return false;   // EOF or error: fewer bytes than asked
        const uint16_t got = _neo_tnfs_get16(t->resp + 5);
        if (got == 0 || got > want || got > n - 2) return false;
        memcpy(buf, t->resp + 7, got);
        buf += got;
        len -= got;
    }
    return true;
}

bool neo_tnfs_write(neo_tnfs_t* t, uint8_t fd, const uint8_t* buf, uint32_t len) {
    while (len) {
        const uint16_t put = (uint16_t)(len < NEO_TNFS_IO_MAX ? len : NEO_TNFS_IO_MAX);
        t->req[4] = fd;
        _neo_tnfs_le16(t->req + 5, put);
        memcpy(t->req + 7, buf, put);
        const int n = _neo_tnfs_call(t, NEO_TNFS_WRITE, 3 + put);
        if (n < 2 || _neo_tnfs_get16(t->resp + 5) != put) return false;
        buf += put;
        len -= put;
    }
    return true;
}

bool neo_tnfs_list(neo_tnfs_t* t, const char* path, neo_tnfs_dir_cb_t cb, void* user) {
    if (strlen(path) + 1 > NEO_TNFS_MSG_MAX - 4) return false;
    if (_neo_tnfs_call(t, NEO_TNFS_OPENDIR, _neo_tnfs_str(t->req + 4, path)) < 1) return false;
    const uint8_t dh = t->resp[5];
    bool ok = true;
    for (;;) {
        t->req[4] = dh;
        const int n = _neo_tnfs_call(t, NEO_TNFS_READDIR, 1);
        if (n < 0) {
            ok = t->error == NEO_TNFS_EOF;
            break;
        }
        char name[NEO_STORAGE_NAME_MAX];
        int k = 0;
        while (k < n && k < (int)sizeof(name) - 1 && t->resp[5 + k]) {
            name[k] = (char)t->resp[5 + k];
            k++;
        }
        name[k] = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!cb(name, user)) break;
    }
    t->req[4] = dh;
    _neo_tnfs_call(t, NEO_TNFS_CLOSEDIR, 1);
    return ok;
}

bool neo_tnfs_unlink(neo_tnfs_t* t, const char* path) {
    if (strlen(path) + 1 > NEO_TNFS_MSG_MAX - 4) return false;
    return _neo_tnfs_call(t, NEO_TNFS_UNLINK, _neo_tnfs_str(t->req + 4, path)) >= 0;
}

// Volume neo_storage : le descripteur est un neo_tnfs_file_t (qui pointe vers son
// client) ; la position du serveur est suivie : un LSEEK seulement quand elle change
static void _neo_tnfs_abs(const char* path, char* out, size_t n) {
    const size_t k = path[0] == '/' ? 0 : 1;
    out[0] = '/';
    strncpy(out + k, path, n - 1 - k);
    out[n - 1] = 0;
}

static bool _neo_tnfs_ready(void* vol) { return ((neo_tnfs_t*)vol)->mounted; }

static void* _neo_tnfs_vopen(void* vol, const char* path, int mode, uint32_t* size) {
    neo_tnfs_t* t = (neo_tnfs_t*)vol;
    int slot = 0;
    while (slot < NEO_TNFS_FILES && t->files[slot].used) slot++;
    if (slot == NEO_TNFS_FILES) return 0;
    char p[NEO_TNFS_MSG_MAX - 16];
    _neo_tnfs_abs(path, p, sizeof(p));
    uint16_t flags = (mode & NEO_WRITE) ? ((mode & NEO_READ) ? NEO_TNFS_O_RDWR : NEO_TNFS_O_WRONLY) : NEO_TNFS_O_RDONLY;
    if (mode & NEO_CREATE) flags |= NEO_TNFS_O_CREAT | NEO_TNFS_O_TRUNC;
    uint32_t sz = 0;
    if (!(mode & NEO_CREATE)) {
        uint16_t m = 0;
        if (!neo_tnfs_stat(t, p, &sz, &m) || (m & NEO_TNFS_S_IFMT) == NEO_TNFS_S_IFDIR) return 0;
    }
    const int fd = neo_tnfs_open(t, p, flags, 0644);
    if (fd < 0) return 0;
    neo_tnfs_file_t* f = &t->files[slot];
    f->owner = t;
    f->used = true;
    f->fd = (uint8_t)fd;
    f->pos = 0;
    *size = sz;
    return f;
}

static bool _neo_tnfs_position(neo_tnfs_file_t* f, uint32_t offset) {
    if (f->pos == offset) return true;
    if (!neo_tnfs_seek(f->owner, f->fd, offset)) return false;
    f->pos = offset;
    return true;
}

static bool _neo_tnfs_vread(void* h, uint32_t offset, uint8_t* buf, uint32_t len) {
    neo_tnfs_file_t* f = (neo_tnfs_file_t*)h;
    if (!_neo_tnfs_position(f, offset)) return false;
    f->pos = 0xFFFFFFFFu;   // Unknown until done (a failed READ may have moved it)
    if (!neo_tnfs_read(f->owner, f->fd, buf, len)) return false;
    f->pos = offset + len;
    return true;
}

static bool _neo_tnfs_vwrite(void* h, uint32_t offset, const uint8_t* buf, uint32_t len) {
    neo_tnfs_file_t* f = (neo_tnfs_file_t*)h;
    if (!_neo_tnfs_position(f, offset)) return false;
    f->pos = 0xFFFFFFFFu;
    if (!neo_tnfs_write(f->owner, f->fd, buf, len)) return false;
    f->pos = offset + len;
    return true;
}

static bool _neo_tnfs_vclose(void* h) {
    neo_tnfs_file_t* f = (neo_tnfs_file_t*)h;
    const bool ok = neo_tnfs_close(f->owner, f->fd);
    f->used = false;
    return ok;
}

// Listing: the names are read first (READDIR up to the end, then CLOSEDIR), then
// each one is STAT'ed for its size and type (mandatory commands only; READDIRX,
// which returns both at once, is optional in the protocol)
#ifndef NEO_TNFS_LIST_MAX
#define NEO_TNFS_LIST_MAX 32
#endif
static char _neo_tnfs_names[NEO_TNFS_LIST_MAX][NEO_STORAGE_NAME_MAX];
static int _neo_tnfs_count;

static bool _neo_tnfs_collect(const char* name, void* user) {
    (void)user;
    if (strlen(name) >= NEO_STORAGE_NAME_MAX) return true;   // Too long to be named: skipped
    if (_neo_tnfs_count >= NEO_TNFS_LIST_MAX) return false;
    strcpy(_neo_tnfs_names[_neo_tnfs_count++], name);
    return true;
}

static bool _neo_tnfs_vlist(void* vol, const char* dir, neo_dirent_cb_t cb, void* user) {
    neo_tnfs_t* t = (neo_tnfs_t*)vol;
    char d[NEO_TNFS_MSG_MAX - 80];
    _neo_tnfs_abs(dir[0] ? dir : "/", d, sizeof(d));
    _neo_tnfs_count = 0;
    if (!neo_tnfs_list(t, d, _neo_tnfs_collect, 0)) return false;
    const size_t dl = strlen(d);
    for (int i = 0; i < _neo_tnfs_count; i++) {
        neo_dirent_t e;
        memset(&e, 0, sizeof(e));
        strcpy(e.name, _neo_tnfs_names[i]);
        e.hidden = e.name[0] == '.';
        char p[NEO_TNFS_MSG_MAX - 16];
        memcpy(p, d, dl);
        size_t k = dl;
        if (p[k - 1] != '/') p[k++] = '/';
        strcpy(p + k, e.name);
        uint16_t m = 0;
        neo_tnfs_stat(t, p, &e.size, &m);
        e.dir = (m & NEO_TNFS_S_IFMT) == NEO_TNFS_S_IFDIR;
        if (!cb(&e, user)) break;
    }
    return true;
}

static bool _neo_tnfs_vremove(void* vol, const char* path) {
    char p[NEO_TNFS_MSG_MAX - 16];
    _neo_tnfs_abs(path, p, sizeof(p));
    return neo_tnfs_unlink((neo_tnfs_t*)vol, p);
}

const neo_storage_ops_t neo_storage_tnfs_ops = {_neo_tnfs_ready, _neo_tnfs_vopen, _neo_tnfs_vread, _neo_tnfs_vwrite,
                                                _neo_tnfs_vclose, _neo_tnfs_vlist, _neo_tnfs_vremove};

#endif  // NEO_TNFS_IMPL
