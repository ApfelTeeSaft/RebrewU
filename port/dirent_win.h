// dirent_win.h — minimal POSIX dirent shim for MSVC
// Wraps Win32 FindFirstFile/FindNextFile to provide opendir/readdir/closedir.
#pragma once
#ifdef _WIN32
#ifndef DIRENT_WIN_H
#define DIRENT_WIN_H

#include <windows.h>
#include <string.h>

struct dirent {
    char d_name[MAX_PATH];
};

struct DIR {
    HANDLE          handle;
    WIN32_FIND_DATAA data;
    struct dirent   entry;
    int             first;   // 1 = FindFirstFile result not yet returned
};

static inline DIR* opendir(const char* path) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (!d) return nullptr;
    d->handle = FindFirstFileA(pattern, &d->data);
    if (d->handle == INVALID_HANDLE_VALUE) { free(d); return nullptr; }
    d->first = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d) {
    if (!d || d->handle == INVALID_HANDLE_VALUE) return nullptr;
    if (d->first) {
        d->first = 0;
    } else {
        if (!FindNextFileA(d->handle, &d->data)) return nullptr;
    }
    strncpy(d->entry.d_name, d->data.cFileName, MAX_PATH - 1);
    d->entry.d_name[MAX_PATH - 1] = '\0';
    return &d->entry;
}

static inline int closedir(DIR* d) {
    if (!d) return -1;
    if (d->handle != INVALID_HANDLE_VALUE) FindClose(d->handle);
    free(d);
    return 0;
}

#endif // DIRENT_WIN_H
#endif // _WIN32
