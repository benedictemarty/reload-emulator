#pragma once

// neo_storage.h — volumes de fichiers communs aux émulateurs Neo6502
//
// Un émulateur n'appelle plus FatFs (ou stdio) directement : il ouvre ses
// fichiers sur un volume numéroté (0 : clé USB ; plus tard : réseau, images en
// flash…), et lit / écrit à une position donnée. Chaque volume est servi par
// un pilote (neo_storage_ops_t) : FatFs sur la carte
// (platforms/rp2040/src/neo_storage_fatfs.c), POSIX sur PC
// (platforms/pc/neo_storage_posix.h), mémoire pour les tests ; un pilote réseau
// (TNFS) s'ajoutera sans toucher aux émulateurs.
//
// Les fonctions neo_file_read_cb / neo_file_write_cb ont la signature des
// lectures en flux du WD1793 (oric_dsk_read_t) et de la cassette
// (oric_tape_read_t) : ctx = le neo_file_t ouvert.
//
// C pur, sans allocation ; testé sur PC (tests/oric/test_neo_storage.c).
//
// ## Licence zlib/libpng
//
// Copyright (c) 2026 bmarty
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the
// use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//     1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software in a
//     product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not
//     be misrepresented as being the original software.
//     3. This notice may not be removed or altered from any source
//     distribution.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NEO_STORAGE_VOLUMES
#define NEO_STORAGE_VOLUMES 2
#endif
#define NEO_STORAGE_NAME_MAX 64   // Nom d'une entrée de répertoire, zéro final compris

// Volumes usuels
#define NEO_VOL_USB 0

// Modes d'ouverture
#define NEO_READ   1
#define NEO_WRITE  2
#define NEO_CREATE 4   // Créer, ou vider si le fichier existe (avec NEO_WRITE)

typedef struct {
    char name[NEO_STORAGE_NAME_MAX];
    uint32_t size;
    bool dir;
    bool hidden;   // Caché ou système
} neo_dirent_t;

// Rappel de neo_storage_list : false arrête le parcours
typedef bool (*neo_dirent_cb_t)(const neo_dirent_t* e, void* user);

// Pilote d'un volume. Les lectures et écritures sont exactes (len octets ou
// échec) ; une écriture est durable à son retour (le pilote synchronise).
typedef struct {
    // Volume utilisable (clé montée, serveur joint…) ; NULL : toujours prêt
    bool (*ready)(void* vol);
    // Ouvre path ; *size = taille ; renvoie un descripteur du pilote, NULL en cas d'échec
    void* (*open)(void* vol, const char* path, int mode, uint32_t* size);
    bool (*read)(void* h, uint32_t offset, uint8_t* buf, uint32_t len);
    bool (*write)(void* h, uint32_t offset, const uint8_t* buf, uint32_t len);
    bool (*close)(void* h);
    // Entrées du répertoire dir ("/" : la racine), dans l'ordre du pilote
    bool (*list)(void* vol, const char* dir, neo_dirent_cb_t cb, void* user);
} neo_storage_ops_t;

typedef struct {
    const char* label;   // Pour le menu : « Clé USB », « Réseau »…
    const neo_storage_ops_t* ops;
    void* ctx;
} neo_volume_t;

typedef struct {
    const neo_volume_t* vol;
    void* h;
    uint32_t size;
    uint32_t pos;    // Position de neo_file_append
    bool writable;
} neo_file_t;

// Volumes
void neo_storage_set(int vol, const char* label, const neo_storage_ops_t* ops, void* ctx);
const neo_volume_t* neo_storage_volume(int vol);   // NULL si le volume n'est pas défini
bool neo_storage_ready(int vol);
bool neo_storage_list(int vol, const char* dir, neo_dirent_cb_t cb, void* user);

// Fichiers
bool neo_file_open(neo_file_t* f, int vol, const char* path, int mode);
static inline bool neo_file_is_open(const neo_file_t* f) { return f->h != 0; }
bool neo_file_read(neo_file_t* f, uint32_t offset, uint8_t* buf, uint32_t len);
bool neo_file_write(neo_file_t* f, uint32_t offset, const uint8_t* buf, uint32_t len);
bool neo_file_append(neo_file_t* f, const uint8_t* buf, uint32_t len);   // À pos, puis pos += len
bool neo_file_close(neo_file_t* f);   // Sans effet sur un fichier fermé

// Rappels de lecture / écriture en flux (signature d'oric_dsk_read_t, oric_tape_read_t) ; ctx = neo_file_t*
bool neo_file_read_cb(void* ctx, uint32_t offset, uint8_t* buf, uint32_t len);
bool neo_file_write_cb(void* ctx, uint32_t offset, const uint8_t* buf, uint32_t len);

// Fichier entier : lit au plus max octets (*n = lus ; false si absent ou plus grand que max
// quand exact) ; écrit (créé ou remplacé)
bool neo_file_load(int vol, const char* path, uint8_t* buf, uint32_t max, uint32_t* n, bool exact);
bool neo_file_save(int vol, const char* path, const uint8_t* buf, uint32_t len);

// Pilote en mémoire (tests ; images intégrées) : un seul répertoire, fichiers en RAM
typedef struct {
    const char* name;
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;   // Taille maximale en écriture (0 : lecture seule)
} neo_mem_file_t;
typedef struct {
    neo_mem_file_t* files;
    int count;
} neo_mem_volume_t;
extern const neo_storage_ops_t neo_storage_mem_ops;

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef NEO_STORAGE_IMPL

static neo_volume_t _neo_volumes[NEO_STORAGE_VOLUMES];

void neo_storage_set(int vol, const char* label, const neo_storage_ops_t* ops, void* ctx) {
    if (vol < 0 || vol >= NEO_STORAGE_VOLUMES) return;
    _neo_volumes[vol].label = label;
    _neo_volumes[vol].ops = ops;
    _neo_volumes[vol].ctx = ctx;
}

const neo_volume_t* neo_storage_volume(int vol) {
    if (vol < 0 || vol >= NEO_STORAGE_VOLUMES || !_neo_volumes[vol].ops) return 0;
    return &_neo_volumes[vol];
}

bool neo_storage_ready(int vol) {
    const neo_volume_t* v = neo_storage_volume(vol);
    return v && (!v->ops->ready || v->ops->ready(v->ctx));
}

bool neo_storage_list(int vol, const char* dir, neo_dirent_cb_t cb, void* user) {
    const neo_volume_t* v = neo_storage_volume(vol);
    return v && v->ops->list && neo_storage_ready(vol) && v->ops->list(v->ctx, dir, cb, user);
}

bool neo_file_open(neo_file_t* f, int vol, const char* path, int mode) {
    memset(f, 0, sizeof(*f));
    const neo_volume_t* v = neo_storage_volume(vol);
    if (!v || !neo_storage_ready(vol)) return false;
    uint32_t size = 0;
    void* h = v->ops->open(v->ctx, path, mode, &size);
    if (!h) return false;
    f->vol = v;
    f->h = h;
    f->size = size;
    f->writable = (mode & NEO_WRITE) != 0;
    return true;
}

bool neo_file_read(neo_file_t* f, uint32_t offset, uint8_t* buf, uint32_t len) {
    if (!f->h || offset > f->size || len > f->size - offset) return false;
    return len == 0 || f->vol->ops->read(f->h, offset, buf, len);
}

bool neo_file_write(neo_file_t* f, uint32_t offset, const uint8_t* buf, uint32_t len) {
    if (!f->h || !f->writable || !f->vol->ops->write) return false;
    if (len && !f->vol->ops->write(f->h, offset, buf, len)) return false;
    if (offset + len > f->size) f->size = offset + len;
    return true;
}

bool neo_file_append(neo_file_t* f, const uint8_t* buf, uint32_t len) {
    if (!neo_file_write(f, f->pos, buf, len)) return false;
    f->pos += len;
    return true;
}

bool neo_file_close(neo_file_t* f) {
    bool ok = true;
    if (f->h) ok = f->vol->ops->close(f->h);
    memset(f, 0, sizeof(*f));
    return ok;
}

bool neo_file_read_cb(void* ctx, uint32_t offset, uint8_t* buf, uint32_t len) {
    return neo_file_read((neo_file_t*)ctx, offset, buf, len);
}

bool neo_file_write_cb(void* ctx, uint32_t offset, const uint8_t* buf, uint32_t len) {
    return neo_file_write((neo_file_t*)ctx, offset, buf, len);
}

bool neo_file_load(int vol, const char* path, uint8_t* buf, uint32_t max, uint32_t* n, bool exact) {
    neo_file_t f;
    *n = 0;
    if (!neo_file_open(&f, vol, path, NEO_READ)) return false;
    bool ok = !(exact && f.size != max);
    uint32_t len = f.size < max ? f.size : max;
    ok = ok && neo_file_read(&f, 0, buf, len);
    neo_file_close(&f);
    if (ok) *n = len;
    return ok;
}

bool neo_file_save(int vol, const char* path, const uint8_t* buf, uint32_t len) {
    neo_file_t f;
    if (!neo_file_open(&f, vol, path, NEO_WRITE | NEO_CREATE)) return false;
    bool ok = neo_file_write(&f, 0, buf, len);
    return neo_file_close(&f) && ok;
}

// Pilote en mémoire
static neo_mem_file_t* _neo_mem_find(neo_mem_volume_t* v, const char* path) {
    while (*path == '/') path++;
    for (int i = 0; i < v->count; i++) {
        if (!strcmp(v->files[i].name, path)) return &v->files[i];
    }
    return 0;
}
static void* _neo_mem_open(void* vol, const char* path, int mode, uint32_t* size) {
    neo_mem_file_t* f = _neo_mem_find((neo_mem_volume_t*)vol, path);
    if (!f || ((mode & NEO_WRITE) && !f->capacity)) return 0;
    if (mode & NEO_CREATE) f->size = 0;
    *size = f->size;
    return f;
}
static bool _neo_mem_read(void* h, uint32_t offset, uint8_t* buf, uint32_t len) {
    neo_mem_file_t* f = (neo_mem_file_t*)h;
    if (offset > f->size || len > f->size - offset) return false;
    memcpy(buf, f->data + offset, len);
    return true;
}
static bool _neo_mem_write(void* h, uint32_t offset, const uint8_t* buf, uint32_t len) {
    neo_mem_file_t* f = (neo_mem_file_t*)h;
    if (offset > f->capacity || len > f->capacity - offset) return false;
    memcpy(f->data + offset, buf, len);
    if (offset + len > f->size) f->size = offset + len;
    return true;
}
static bool _neo_mem_close(void* h) {
    (void)h;
    return true;
}
static bool _neo_mem_list(void* vol, const char* dir, neo_dirent_cb_t cb, void* user) {
    neo_mem_volume_t* v = (neo_mem_volume_t*)vol;
    if (strcmp(dir, "/") && strcmp(dir, "")) return false;
    for (int i = 0; i < v->count; i++) {
        neo_dirent_t e;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, v->files[i].name, sizeof(e.name) - 1);
        e.size = v->files[i].size;
        if (!cb(&e, user)) break;
    }
    return true;
}
const neo_storage_ops_t neo_storage_mem_ops = {0, _neo_mem_open, _neo_mem_read, _neo_mem_write, _neo_mem_close,
                                               _neo_mem_list};

#endif  // NEO_STORAGE_IMPL
