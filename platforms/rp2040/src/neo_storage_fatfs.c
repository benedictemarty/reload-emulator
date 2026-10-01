// neo_storage_fatfs.c — pilote neo_storage sur FatFs (clé USB, msc_app.c)
//
// Volume : ctx = préfixe de lecteur FatFs ("" : lecteur par défaut, "1:"…).
//   neo_storage_set(NEO_VOL_USB, "Clé USB", &neo_storage_fatfs_ops, "");
// Fichiers ouverts en même temps : NEO_FATFS_FILES (une réserve de FIL, pas
// d'allocation). Prêt : à l'appelant de ne servir le volume qu'une fois la clé
// montée (ready est NULL ; msc_app.c n'expose pas d'état commun à tous les ports).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "../../../src/devices/neo_storage.h"   // Relatif : les consommateurs (RELOAD_DIR) n'ont pas src/ dans leurs chemins

#ifndef NEO_FATFS_FILES
#define NEO_FATFS_FILES 6   // 4 lecteurs + cassette + enregistrement / configuration
#endif

static FIL neo_fatfs_fil[NEO_FATFS_FILES];
static bool neo_fatfs_used[NEO_FATFS_FILES];

static void neo_fatfs_path(const char *drive, const char *path, char *out, size_t n) {
    snprintf(out, n, "%s%s", drive ? drive : "", path);
}

static void *neo_fatfs_open(void *vol, const char *path, int mode, uint32_t *size) {
    int i = 0;
    while (i < NEO_FATFS_FILES && neo_fatfs_used[i]) i++;
    if (i == NEO_FATFS_FILES) return 0;
    char p[96];
    neo_fatfs_path((const char *)vol, path, p, sizeof(p));
    BYTE m = (mode & NEO_READ) ? FA_READ : 0;
    if (mode & NEO_WRITE) m |= FA_WRITE | ((mode & NEO_CREATE) ? FA_CREATE_ALWAYS : 0);
    if (f_open(&neo_fatfs_fil[i], p, m) != FR_OK) return 0;
    neo_fatfs_used[i] = true;
    *size = (uint32_t)f_size(&neo_fatfs_fil[i]);
    return &neo_fatfs_fil[i];
}

static bool neo_fatfs_read(void *h, uint32_t offset, uint8_t *buf, uint32_t len) {
    UINT n = 0;
    return f_lseek((FIL *)h, offset) == FR_OK && f_read((FIL *)h, buf, len, &n) == FR_OK && n == len;
}

static bool neo_fatfs_write(void *h, uint32_t offset, const uint8_t *buf, uint32_t len) {
    UINT n = 0;
    return f_lseek((FIL *)h, offset) == FR_OK && f_write((FIL *)h, buf, len, &n) == FR_OK && n == len &&
           f_sync((FIL *)h) == FR_OK;
}

static bool neo_fatfs_close(void *h) {
    FIL *f = (FIL *)h;
    bool ok = f_close(f) == FR_OK;
    neo_fatfs_used[f - neo_fatfs_fil] = false;
    return ok;
}

static bool neo_fatfs_list(void *vol, const char *dir, neo_dirent_cb_t cb, void *user) {
    char p[96];
    neo_fatfs_path((const char *)vol, dir, p, sizeof(p));
    DIR d;
    FILINFO fno;
    if (f_opendir(&d, p) != FR_OK) return false;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        const size_t len = strlen(fno.fname);
        if (len >= NEO_STORAGE_NAME_MAX) continue;   // Too long to be named: skipped, not truncated
        neo_dirent_t e;
        memset(&e, 0, sizeof(e));
        memcpy(e.name, fno.fname, len);
        e.size = (uint32_t)fno.fsize;
        e.dir = (fno.fattrib & AM_DIR) != 0;
        e.hidden = (fno.fattrib & (AM_HID | AM_SYS)) != 0;
        if (!cb(&e, user)) break;
    }
    f_closedir(&d);
    return true;
}

static bool neo_fatfs_remove(void *vol, const char *path) {
    char p[96];
    neo_fatfs_path((const char *)vol, path, p, sizeof(p));
    return f_unlink(p) == FR_OK;
}

const neo_storage_ops_t neo_storage_fatfs_ops = {0, neo_fatfs_open, neo_fatfs_read, neo_fatfs_write, neo_fatfs_close,
                                                 neo_fatfs_list, neo_fatfs_remove};
