#include "hvsparse.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (8u << 20)

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w, n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r, n -= (size_t)r;
    }
    return 0;
}

static int all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

static int copy_raw_runs(const char *src, const char *dst, bool packed,
                         void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    int in = open(src, O_RDONLY | O_NONBLOCK), out = -1;
    struct stat st;
    uint8_t *buf = malloc(CHUNK);
    int rc = -1;
    if (in < 0 || fstat(in, &st) != 0 || !S_ISREG(st.st_mode) || !buf) goto done;

    /* The caller chooses a new destination. Opening exclusively protects the source
       as well, including when src and dst are aliases of the same file. */
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) goto done;
    uint64_t size = (uint64_t)st.st_size;
    if (packed) {
        if (write_all(out, "HVSPARSE", 8) || write_all(out, &size, 8)) goto done;
    } else if (ftruncate(out, st.st_size) != 0) goto done;

    /* Walk data runs and skip zero chunks. Fall back to sequential inspection
       when an imported disk lives on a provider without SEEK_DATA support. */
    bool sparse_seek = true;
    for (off_t pos = 0; pos < st.st_size;) {
        off_t data = sparse_seek ? lseek(in, pos, SEEK_DATA) : pos;
        if (data < 0) {
            if (errno == ENXIO) break;
            if (errno != EINVAL && errno != ENOTSUP) goto done;
            sparse_seek = false;
            data = pos;
        }
        off_t hole = sparse_seek ? lseek(in, data, SEEK_HOLE) : st.st_size;
        if (data < pos || hole <= data || hole > st.st_size) goto done;
        for (off_t at = data; at < hole;) {
            size_t n = (size_t)(hole - at < CHUNK ? hole - at : CHUNK);
            if (pread(in, buf, n, at) != (ssize_t)n) goto done;
            if (!all_zero(buf, n)) {
                if (packed) {
                    uint64_t rec[2] = {(uint64_t)at, n};
                    if (write_all(out, rec, 16) || write_all(out, buf, n)) goto done;
                } else if (pwrite(out, buf, n, at) != (ssize_t)n) goto done;
            }
            at += (off_t)n;
            if (progress) progress((uint64_t)at, size, opaque);
        }
        pos = hole;
    }
    if (packed) {
        uint64_t end[2] = {0, 0};
        if (write_all(out, end, 16)) goto done;
    }
    rc = fsync(out);
done:
    free(buf);
    if (in >= 0) close(in);
    if (out >= 0) {
        if (close(out) != 0) rc = -1;
        if (rc != 0) unlink(dst);
    }
    return rc;
}

int hv_sparse_pack_progress(const char *src, const char *dst, void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    return copy_raw_runs(src, dst, true, progress, opaque);
}

int hv_sparse_copy_raw_progress(const char *src, const char *dst,
                                void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    return copy_raw_runs(src, dst, false, progress, opaque);
}

int hv_sparse_copy_raw(const char *src, const char *dst) {
    return hv_sparse_copy_raw_progress(src, dst, NULL, NULL);
}

int hv_sparse_pack(const char *src, const char *dst) { return hv_sparse_pack_progress(src, dst, NULL, NULL); }

static int read_sparse(const char *src, const char *dst, uint64_t minimum_size,
                       void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    int in = open(src, O_RDONLY | O_NONBLOCK), out = -1;
    uint8_t *buf = NULL;
    char *temp = NULL;
    bool created = false;
    int rc = -1;
    char magic[8];
    uint64_t size;
    struct stat st;
    if (in < 0 || fstat(in, &st) != 0 || !S_ISREG(st.st_mode) ||
        read_all(in, magic, 8) || memcmp(magic, "HVSPARSE", 8) ||
        read_all(in, &size, 8) || size < minimum_size || size > INT64_MAX) goto done;

    if (dst) {
        size_t temp_len = strlen(dst) + sizeof ".import.XXXXXX";
        temp = malloc(temp_len);
        buf = malloc(CHUNK);
        if (!temp || !buf) goto done;
        snprintf(temp, temp_len, "%s.import.XXXXXX", dst);
        out = mkstemp(temp);
        if (out < 0) goto done;
        created = true;
        if (ftruncate(out, (off_t)size) != 0) goto done;
    }

    uint64_t previous_end = 0;
    for (;;) {
        uint64_t rec[2];
        if (read_all(in, rec, 16)) goto done;
        if (!rec[1]) {
            if (rec[0] != 0 || lseek(in, 0, SEEK_CUR) != st.st_size) goto done;
            break;
        }
        if (rec[1] > CHUNK || rec[0] < previous_end || rec[0] > size || rec[1] > size - rec[0]) goto done;
        off_t input_offset = lseek(in, 0, SEEK_CUR);
        if (input_offset < 0 || input_offset > st.st_size ||
            rec[1] > (uint64_t)(st.st_size - input_offset)) goto done;
        if (dst) {
            if (read_all(in, buf, rec[1]) ||
                pwrite(out, buf, rec[1], (off_t)rec[0]) != (ssize_t)rec[1]) goto done;
        } else if (lseek(in, (off_t)rec[1], SEEK_CUR) < 0) goto done;
        previous_end = rec[0] + rec[1];
        if (progress) progress(previous_end, size, opaque);
    }
    if (!dst) {
        rc = 0;
        goto done;
    }
    if (fsync(out) != 0) goto done;
    int close_result = close(out);
    out = -1;
    if (close_result != 0) goto done;
    /* Publish atomically beside the temporary file and refuse to replace an existing disk. */
    rc = link(temp, dst);
done:
    free(buf);
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    if (created) unlink(temp);
    free(temp);
    return rc;
}


int hv_sparse_unpack(const char *src, const char *dst, uint64_t minimum_size,
                     void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    return read_sparse(src, dst, minimum_size, progress, opaque);
}

int hv_sparse_validate(const char *src, uint64_t minimum_size) {
    return read_sparse(src, NULL, minimum_size, NULL, NULL);
}
