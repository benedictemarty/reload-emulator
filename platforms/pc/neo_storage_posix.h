#pragma once

// neo_storage_posix.h — pilote neo_storage sur un dossier du PC (stdio, dirent)
//
// Volume = un dossier racine (ctx : chaîne, "" pour des chemins tels quels) ;
// sert à oric_headless et aux tests (fonctions statiques).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include "devices/neo_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static void _neo_posix_path(const char* root, const char* path, char* out, size_t n) {
    if (root && root[0]) {
        while (*path == '/') path++;
        snprintf(out, n, "%s/%s", root, path);
    } else {
        snprintf(out, n, "%s", path);
    }
}

static void* _neo_posix_open(void* vol, const char* path, int mode, uint32_t* size) {
    char p[1024];
    _neo_posix_path((const char*)vol, path, p, sizeof(p));
    const char* m = (mode & NEO_WRITE) ? ((mode & NEO_CREATE) ? "w+b" : "r+b") : "rb";
    FILE* f = fopen(p, m);
    if (!f) return 0;
    struct stat st;
    *size = fstat(fileno(f), &st) == 0 ? (uint32_t)st.st_size : 0;
    return f;
}

static bool _neo_posix_read(void* h, uint32_t offset, uint8_t* buf, uint32_t len) {
    FILE* f = (FILE*)h;
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

static bool _neo_posix_write(void* h, uint32_t offset, const uint8_t* buf, uint32_t len) {
    FILE* f = (FILE*)h;
    return fseek(f, (long)offset, SEEK_SET) == 0 && fwrite(buf, 1, len, f) == len && fflush(f) == 0;
}

static bool _neo_posix_close(void* h) { return fclose((FILE*)h) == 0; }

static bool _neo_posix_list(void* vol, const char* dir, neo_dirent_cb_t cb, void* user) {
    char p[1024];
    _neo_posix_path((const char*)vol, dir, p, sizeof(p));
    DIR* d = opendir(p[0] ? p : ".");
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d)) != 0) {
        if (de->d_name[0] == '.') continue;
        const size_t len = strlen(de->d_name);
        if (len >= NEO_STORAGE_NAME_MAX) continue;   // Too long to be named: skipped, not truncated
        neo_dirent_t e;
        memset(&e, 0, sizeof(e));
        memcpy(e.name, de->d_name, len);
        char q[1300];
        snprintf(q, sizeof(q), "%s/%s", p[0] ? p : ".", de->d_name);
        struct stat st;
        if (stat(q, &st) == 0) {
            e.size = (uint32_t)st.st_size;
            e.dir = S_ISDIR(st.st_mode);
        }
        if (!cb(&e, user)) break;
    }
    closedir(d);
    return true;
}

static const neo_storage_ops_t neo_storage_posix_ops = {0, _neo_posix_open, _neo_posix_read, _neo_posix_write,
                                                        _neo_posix_close, _neo_posix_list};
