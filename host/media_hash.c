/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "media_hash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

int media_sha256(const char *path, char hex[65], MediaHashProgress progress, void *context) {
    enum { CHUNK_SIZE = 1 << 20 };
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    if (!path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    unsigned char *buffer = malloc(CHUNK_SIZE);
    struct stat info;
    int result = -1;
#ifdef __APPLE__
    CC_SHA256_CTX sha;
    if (!CC_SHA256_Init(&sha)) goto done;
#else
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    if (!sha || !EVP_DigestInit_ex(sha, EVP_sha256(), NULL)) goto done;
#endif
    if (!buffer || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) goto done;
    uint64_t total = (uint64_t)info.st_size, consumed = 0;
    while (true) {
        ssize_t count = read(fd, buffer, CHUNK_SIZE);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (uint64_t)count > total - consumed) goto done;
        if (!count) break;
#ifdef __APPLE__
        if (!CC_SHA256_Update(&sha, buffer, (CC_LONG)count)) goto done;
#else
        if (!EVP_DigestUpdate(sha, buffer, (size_t)count)) goto done;
#endif
        consumed += (uint64_t)count;
        if (progress && progress(consumed, total, context)) goto done;
    }
    if (consumed != total) goto done;
#ifdef __APPLE__
    if (!CC_SHA256_Final(digest, &sha)) goto done;
#else
    if (!EVP_DigestFinal_ex(sha, digest, NULL)) goto done;
#endif
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    result = 0;
done:
#ifndef __APPLE__
    EVP_MD_CTX_free(sha);
#endif
    free(buffer);
    close(fd);
    return result;
}
