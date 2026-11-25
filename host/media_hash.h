/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_MEDIA_HASH_H
#define WINIOS_MEDIA_HASH_H

#include <stdint.h>

typedef int (*MediaHashProgress)(uint64_t read, uint64_t total, void *context);

/* Hash the complete regular file into 64 lowercase hex digits and a NUL. */
int media_sha256(const char *path, char hex[65], MediaHashProgress progress, void *context);

#endif
