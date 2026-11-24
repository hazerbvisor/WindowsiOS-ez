/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_DISC_IMAGE_H
#define WINIOS_DISC_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct DiscImage DiscImage;
typedef struct DiscDirectory DiscDirectory;
typedef struct DiscFile DiscFile;

typedef enum { DISC_OTHER, DISC_DIR, DISC_REG } DiscEntryType;
typedef struct {
    const char *name;
    DiscEntryType type;
} DiscEntry;

/* Select the first filesystem with a complete Windows setup layout. */
DiscImage *disc_open_windows(const char *path, char *diagnostic, size_t capacity);
void disc_close(DiscImage *image);
const char *disc_filesystem(const DiscImage *image);
DiscDirectory *disc_root(DiscImage *image);
DiscDirectory *disc_opendir_at(DiscDirectory *dir, const char *name);
void disc_closedir(DiscDirectory *dir);
void disc_rewinddir(DiscDirectory *dir);
/* 1 for an entry, 0 at EOF, -1 for a malformed directory or read failure. */
int disc_readdir(DiscDirectory *dir, DiscEntry *entry);
DiscFile *disc_open_at(DiscDirectory *dir, const char *name);
DiscFile *disc_open_path(DiscImage *image, const char *path);
int64_t disc_file_size(const DiscFile *file);
ssize_t disc_file_read(DiscFile *file, void *buffer, size_t size);
ssize_t disc_file_pread(DiscFile *file, void *buffer, size_t size, uint64_t offset);
void disc_file_close(DiscFile *file);

#endif
