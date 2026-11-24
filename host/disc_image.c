/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "disc_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <udfread/blockinput.h>
#include <udfread/udfread.h>

enum { SECTOR = 2048, FIRST_DESCRIPTOR = 16, ANCHOR = 256, MAX_SECTIONS = 4096 };

typedef struct {
    uint64_t offset;
    uint32_t size;
} DiscSection;

typedef struct {
    uint32_t blocks;
    DiscSection root;
    bool seen, valid;
} ISODescriptor;

struct DiscImage {
    udfread_block_input input;
    int fd;
    uint32_t blocks, iso_blocks;
    udfread *udf;
    uint32_t first_failed_lba;
    bool block_failure, joliet;
    char udf_error[128];
    uint64_t malformed_offset;
    bool malformed_directory;
    DiscSection root;
};

struct DiscDirectory {
    DiscImage *image;
    UDFDIR *udf;
    DiscSection extent;
    uint32_t position, buffered_sector;
    uint8_t sector[SECTOR];
    char name[4 * 222 + 1];
    DiscSection first, *sections;
    size_t count, capacity;
    DiscEntryType type;
};

struct DiscFile {
    DiscImage *image;
    UDFFILE *udf;
    uint64_t size, position;
    size_t count;
    DiscSection sections[];
};

static int read_exact(int fd, void *buffer, size_t size, uint64_t offset) {
    uint8_t *bytes = buffer;
    while (size) {
        ssize_t read = pread(fd, bytes, size, (off_t)offset);
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) return -1;
        size -= (size_t)read;
        bytes += read;
        offset += (uint64_t)read;
    }
    return 0;
}

static uint32_t block_count(udfread_block_input *input) {
    return ((DiscImage *)input)->blocks;
}

static int read_blocks(udfread_block_input *input, uint32_t lba, void *buffer, uint32_t count, int flags) {
    (void)flags;
    DiscImage *image = (DiscImage *)input;
    if (lba <= image->blocks && count <= image->blocks - lba && count <= INT32_MAX &&
        !read_exact(image->fd, buffer, (size_t)count * SECTOR, (uint64_t)lba * SECTOR))
        return (int)count;
    if (!image->block_failure) image->first_failed_lba = lba;
    image->block_failure = true;
    return -1;
}

static int capture_udf_error(void *context, const char *format, ...) {
    DiscImage *image = context;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(image->udf_error, sizeof(image->udf_error), format, args);
    va_end(args);
    image->udf_error[strcspn(image->udf_error, "\r\n")] = 0;
    return count;
}

static bool dual16(const uint8_t *data, uint16_t expected) {
    return (data[0] | data[1] << 8) == expected && (data[3] | data[2] << 8) == expected;
}

static bool dual32(const uint8_t *data, uint32_t *value) {
    uint32_t little = 0, big = 0;
    for (unsigned i = 0; i < 4; ++i) {
        little |= (uint32_t)data[i] << (8 * i);
        big = (big << 8) | data[4 + i];
    }
    *value = little;
    return little == big;
}

static bool section_from_record(const uint8_t *record, uint32_t blocks, DiscSection *section) {
    uint32_t lba, size;
    /* Both byte-order copies must agree. Interleaved and multi-volume files are unsupported. */
    if (!dual32(record + 2, &lba) || !dual32(record + 10, &size) ||
        !dual16(record + 28, 1) || record[26] || record[27]) return false;
    uint64_t offset = ((uint64_t)lba + record[1]) * SECTOR;
    if (offset + size > (uint64_t)blocks * SECTOR) return false;
    *section = (DiscSection){offset, size};
    return true;
}

static void read_descriptor(ISODescriptor *descriptor, const uint8_t *sector, uint32_t image_blocks) {
    descriptor->seen = true;
    uint32_t blocks;
    const uint8_t *root = sector + 156;
    descriptor->valid = sector[6] == 1 && dual32(sector + 80, &blocks) &&
        blocks && blocks <= image_blocks && dual16(sector + 128, SECTOR) &&
        root[0] == 34 && root[25] == 2 && root[32] == 1 && root[33] == 0 &&
        section_from_record(root, blocks, &descriptor->root) && descriptor->root.size >= 34;
    if (descriptor->valid) descriptor->blocks = blocks;
}

static DiscDirectory *new_directory(DiscImage *image, const DiscSection *section, UDFDIR *udf) {
    DiscDirectory *dir = calloc(1, sizeof(*dir));
    if (!dir) {
        if (udf) udfread_closedir(udf);
        return NULL;
    }
    dir->image = image;
    dir->udf = udf;
    dir->buffered_sector = UINT32_MAX;
    if (section) dir->extent = *section;
    return dir;
}

DiscDirectory *disc_root(DiscImage *image) {
    if (!image->udf) return new_directory(image, &image->root, NULL);
    UDFDIR *dir = udfread_opendir(image->udf, "/");
    return dir ? new_directory(image, NULL, dir) : NULL;
}

void disc_closedir(DiscDirectory *dir) {
    if (!dir) return;
    if (dir->udf) udfread_closedir(dir->udf);
    free(dir->sections);
    free(dir);
}

void disc_rewinddir(DiscDirectory *dir) {
    if (dir->udf) udfread_rewinddir(dir->udf);
    dir->position = 0;
    dir->count = 0;
    dir->name[0] = 0;
}

static int next_record(DiscDirectory *dir, const uint8_t **record) {
    while (dir->position < dir->extent.size) {
        uint32_t sector = dir->position / SECTOR;
        unsigned index = dir->position % SECTOR;
        if (sector != dir->buffered_sector) {
            size_t remaining = dir->extent.size - (uint64_t)sector * SECTOR;
            size_t count = remaining < SECTOR ? remaining : SECTOR;
            if (read_exact(dir->image->fd, dir->sector, count, dir->extent.offset + (uint64_t)sector * SECTOR))
                return -1;
            dir->buffered_sector = sector;
        }
        const uint8_t *data = dir->sector + index;
        unsigned length = data[0];
        if (!length) {
            /* A directory record never crosses a logical sector. */
            unsigned padding = SECTOR - index;
            if (dir->extent.size - dir->position <= padding) return 0;
            dir->position += padding;
            continue;
        }
        if (length < 34 || length > SECTOR - index || length > dir->extent.size - dir->position ||
            data[32] == 0 || 33u + data[32] + (data[32] % 2 == 0) > length)
            return -1;
        dir->position += length;
        *record = data;
        return 1;
    }
    return 0;
}

static bool decode_name(char *name, const uint8_t *data, unsigned length, bool joliet, bool is_directory) {
    char *out = name;
    if (joliet && length % 2) return false;
    for (unsigned i = 0; i < length;) {
        unsigned character = data[i++];
        if (joliet) character = (character << 8) | data[i++];
        if (!character || character == '/' || (character >= 0xd800 && character <= 0xdfff) ||
            (!joliet && character >= 128)) return false;
        if (character >= 0x800) *out++ = (char)(0xe0 | (character >> 12));
        if (character >= 0x80) *out++ = (char)((character >= 0x800 ? 0x80 : 0xc0) | ((character >> 6) & 0x3f));
        *out++ = (char)(character >= 0x80 ? 0x80 | (character & 0x3f) : character);
    }
    *out = 0;
    if (!is_directory) {
        char *version = strrchr(name, ';');
        if (version && version[1] && version[strspn(version + 1, "0123456789") + 1] == 0)
            *version = 0;
        size_t size = strlen(name);
        if (size && name[size - 1] == '.') name[size - 1] = 0;
    }
    return name[0] && strcmp(name, ".") && strcmp(name, "..");
}

static bool append_section(DiscDirectory *dir, DiscSection section) {
    if (dir->count == MAX_SECTIONS) return false;
    if (!dir->count) {
        dir->first = section;
    } else {
        if (dir->count >= dir->capacity) {
            size_t capacity = dir->capacity ? dir->capacity * 2 : 4;
            DiscSection *sections = realloc(dir->sections, capacity * sizeof(section));
            if (!sections) return false;
            dir->sections = sections;
            dir->capacity = capacity;
        }
        dir->sections[0] = dir->first;
        dir->sections[dir->count] = section;
    }
    ++dir->count;
    return true;
}

static int read_directory(DiscDirectory *dir, DiscEntry *entry) {
    if (dir->udf) {
        struct udfread_dirent udf_entry;
        if (!udfread_readdir(dir->udf, &udf_entry)) return 0;
        entry->name = udf_entry.d_name;
        entry->type = udf_entry.d_type == UDF_DT_DIR ? DISC_DIR :
            (udf_entry.d_type == UDF_DT_REG ? DISC_REG : DISC_OTHER);
        return 1;
    }

    int status;
    const uint8_t *record;
    dir->count = 0;
    dir->name[0] = 0;
    while ((status = next_record(dir, &record)) == 1) {
        unsigned length = record[32];
        if (length == 1 && record[33] <= 1) continue;
        bool directory = (record[25] & 2) != 0;
        if (!decode_name(dir->name, record + 33, length, dir->image->joliet, directory)) return -1;
        dir->type = directory ? DISC_DIR : ((record[25] & 4) ? DISC_OTHER : DISC_REG);
        uint8_t identifier[255];
        memcpy(identifier, record + 33, length);
        /* ISO9660 level 3 stores each section of a large file in the next record. */
        while (true) {
            bool more = (record[25] & 0x80) != 0;
            DiscSection section;
            if (!section_from_record(record, dir->image->iso_blocks, &section) ||
                (directory && (more || section.size < 34)) ||
                (more && (!section.size || section.size % SECTOR)) ||
                !append_section(dir, section)) return -1;
            if (!more) break;
            uint8_t flags = record[25] & 0x7f;
            if (next_record(dir, &record) != 1 || record[32] != length ||
                memcmp(record + 33, identifier, length) || (record[25] & 0x7f) != flags)
                return -1;
        }
        *entry = (DiscEntry){dir->name, dir->type};
        return 1;
    }
    return status;
}

int disc_readdir(DiscDirectory *dir, DiscEntry *entry) {
    int status = read_directory(dir, entry);
    if (status < 0 && !dir->udf) {
        dir->image->malformed_offset = dir->extent.offset + dir->position;
        dir->image->malformed_directory = true;
    }
    return status;
}

static bool find_entry(DiscDirectory *dir, const char *name, DiscEntryType type) {
    if (!dir->udf && dir->count && dir->type == type && !strcasecmp(dir->name, name)) return true;
    disc_rewinddir(dir);
    DiscEntry entry;
    while (disc_readdir(dir, &entry) == 1) {
        if (entry.name && entry.type == type && !strcasecmp(entry.name, name)) {
            if (dir->udf) snprintf(dir->name, sizeof(dir->name), "%s", entry.name);
            return true;
        }
    }
    return false;
}

DiscDirectory *disc_opendir_at(DiscDirectory *dir, const char *name) {
    if (dir->udf) {
        UDFDIR *child = udfread_opendir_at(dir->udf, name);
        return child ? new_directory(dir->image, NULL, child) : NULL;
    }
    return find_entry(dir, name, DISC_DIR) ? new_directory(dir->image, &dir->first, NULL) : NULL;
}

DiscFile *disc_open_at(DiscDirectory *dir, const char *name) {
    if (!dir->udf && !find_entry(dir, name, DISC_REG)) return NULL;
    DiscFile *file = calloc(1, sizeof(*file) + dir->count * sizeof(DiscSection));
    if (!file) return NULL;
    file->image = dir->image;
    if (dir->udf) {
        file->udf = udfread_file_openat(dir->udf, name);
        int64_t size = file->udf ? udfread_file_size(file->udf) : -1;
        if (size < 0 || (uint64_t)size > (uint64_t)dir->image->blocks * SECTOR) {
            disc_file_close(file);
            return NULL;
        }
        file->size = (uint64_t)size;
    } else {
        file->count = dir->count;
        memcpy(file->sections, dir->count == 1 ? &dir->first : dir->sections, dir->count * sizeof(DiscSection));
        for (size_t i = 0; i < file->count; ++i) file->size += file->sections[i].size;
        if (file->size > (uint64_t)dir->image->iso_blocks * SECTOR) {
            disc_file_close(file);
            return NULL;
        }
    }
    return file;
}

DiscFile *disc_open_path(DiscImage *image, const char *path) {
    DiscDirectory *dir = disc_root(image);
    DiscFile *file = NULL;
    while (dir) {
        const char *separator = strchr(path, '/');
        size_t length = separator ? (size_t)(separator - path) : strlen(path);
        char name[256];
        if (!length || length >= sizeof(name)) break;
        memcpy(name, path, length);
        name[length] = 0;
        if (!find_entry(dir, name, separator ? DISC_DIR : DISC_REG)) break;
        if (!separator) {
            /* libudfread lookup is case-sensitive. Use the actual entry spelling. */
            file = disc_open_at(dir, dir->name);
            break;
        }
        DiscDirectory *next = disc_opendir_at(dir, dir->name);
        disc_closedir(dir);
        dir = next;
        path = separator + 1;
    }
    disc_closedir(dir);
    return file;
}

int64_t disc_file_size(const DiscFile *file) {
    return (int64_t)file->size;
}

ssize_t disc_file_pread(DiscFile *file, void *buffer, size_t size, uint64_t offset) {
    if (offset > file->size || size > file->size - offset || size > (SIZE_MAX >> 1)) return -1;
    if (file->udf) {
        if (udfread_file_seek(file->udf, (int64_t)offset, UDF_SEEK_SET) != (int64_t)offset) return -1;
        return udfread_file_read(file->udf, buffer, size);
    }
    size_t remaining = size;
    for (size_t i = 0; i < file->count && remaining; ++i) {
        DiscSection section = file->sections[i];
        if (offset >= section.size) {
            offset -= section.size;
            continue;
        }
        size_t piece = section.size - offset;
        if (piece > remaining) piece = remaining;
        if (read_exact(file->image->fd, buffer, piece, section.offset + offset)) return -1;
        buffer = (uint8_t *)buffer + piece;
        remaining -= piece;
        offset = 0;
    }
    return remaining ? -1 : (ssize_t)size;
}

ssize_t disc_file_read(DiscFile *file, void *buffer, size_t size) {
    ssize_t count = disc_file_pread(file, buffer, size, file->position);
    if (count > 0) file->position += (uint64_t)count;
    return count;
}

void disc_file_close(DiscFile *file) {
    if (!file) return;
    if (file->udf) udfread_file_close(file->udf);
    free(file);
}

const char *disc_filesystem(const DiscImage *image) {
    return image->udf ? "UDF" : (image->joliet ? "Joliet" : "ISO9660");
}

static bool contains_file(DiscImage *image, const char *path) {
    DiscFile *file = disc_open_path(image, path);
    bool found = file && disc_file_size(file) > 0;
    disc_file_close(file);
    return found;
}

static const char *check_layout(DiscImage *image) {
    if (!contains_file(image, "efi/boot/bootaa64.efi")) return "missing EFI/BOOT/BOOTAA64.EFI";
    if (!contains_file(image, "efi/microsoft/boot/bcd") && !contains_file(image, "boot/bcd"))
        return "missing boot BCD";
    if (!contains_file(image, "sources/boot.wim")) return "missing sources/boot.wim";
    bool wim = contains_file(image, "sources/install.wim");
    bool esd = contains_file(image, "sources/install.esd");
    if (wim == esd) return wim ? "contains both install.wim and install.esd" : "missing sources/install.wim or install.esd";
    return NULL;
}

DiscImage *disc_open_windows(const char *path, char *diagnostic, size_t capacity) {
    if (!path) return NULL;
    DiscImage *image = calloc(1, sizeof(*image));
    if (!image) {
        snprintf(diagnostic, capacity, "Unable to allocate the disc reader.");
        return NULL;
    }
    image->fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (image->fd < 0 || fstat(image->fd, &info)) {
        snprintf(diagnostic, capacity, "Unable to open the ISO file (errno %d).", errno);
        goto failure;
    }
    if (!S_ISREG(info.st_mode)) {
        snprintf(diagnostic, capacity, "The ISO must be a regular file.");
        goto failure;
    }
    if (info.st_size < (FIRST_DESCRIPTOR + 2) * SECTOR || info.st_size % SECTOR ||
        info.st_size / SECTOR > UINT32_MAX) {
        snprintf(diagnostic, capacity, "Invalid disc image length: %lld bytes for %d-byte sectors.",
                 (long long)info.st_size, SECTOR);
        goto failure;
    }
    image->blocks = (uint32_t)(info.st_size / SECTOR);

    ISODescriptor primary = {0}, joliet = {0};
    unsigned joliet_level = 0;
    bool bea = false, nsr2 = false, nsr3 = false, tea = false, iso_end = false;
    uint8_t sector[SECTOR];
    for (uint32_t lba = FIRST_DESCRIPTOR; lba < ANCHOR && lba < image->blocks; ++lba) {
        if (read_exact(image->fd, sector, sizeof(sector), (uint64_t)lba * SECTOR)) {
            snprintf(diagnostic, capacity, "Unable to read disc volume descriptor at sector %u.", lba);
            goto failure;
        }
        if (!memcmp(sector + 1, "CD001", 5) && !iso_end) {
            if (sector[0] == 1 && !primary.valid) read_descriptor(&primary, sector, image->blocks);
            if (sector[0] == 255) iso_end = sector[6] == 1;
            const char *levels = "@CE";
            const char *level = sector[90] ? strchr(levels, sector[90]) : NULL;
            if (sector[0] == 2 && !memcmp(sector + 88, "%/", 2) && level &&
                (unsigned)(level - levels + 1) > joliet_level) {
                ISODescriptor candidate = {0};
                read_descriptor(&candidate, sector, image->blocks);
                if (candidate.valid || !joliet.valid) joliet = candidate;
                if (candidate.valid) joliet_level = (unsigned)(level - levels + 1);
            }
        }
        if (!memcmp(sector + 1, "BEA01", 5)) bea = true;
        if (!memcmp(sector + 1, "NSR02", 5)) nsr2 = true;
        if (!memcmp(sector + 1, "NSR03", 5)) nsr3 = true;
        if (!memcmp(sector + 1, "TEA01", 5)) tea = true;
    }

    char udf_reason[180] = "no UDF recognition descriptors";
    const char *udf_status = udf_reason;
    char joliet_reason[96], iso_reason[96];
    const char *joliet_status = joliet.valid ? "descriptor present" :
        (joliet.seen ? "invalid volume descriptor" : "no supplementary descriptor");
    const char *iso_status = primary.valid ? "descriptor present" :
        (primary.seen ? "invalid volume descriptor" : "no primary descriptor");
    bool selected = false;
    if (bea || nsr2 || nsr3 || tea) {
        udf_status = "libudfread rejected the volume";
        image->input.read = read_blocks;
        image->input.size = block_count;
        image->udf = udfread_init();
        if (image->udf) {
            udfread_set_log(image->udf, image, capture_udf_error);
            if (udfread_open_input(image->udf, &image->input) == 0) {
                udf_status = check_layout(image);
                selected = udf_status == NULL;
            }
            if (!selected && image->udf_error[0]) {
                snprintf(udf_reason, sizeof(udf_reason), "libudfread: %s", image->udf_error);
                udf_status = udf_reason;
            }
        }
        if (!selected && image->udf) {
            udfread_close(image->udf);
            image->udf = NULL;
        }
    }
    if (!selected && joliet.valid && iso_end) {
        image->joliet = true;
        image->root = joliet.root;
        image->iso_blocks = joliet.blocks;
        joliet_status = check_layout(image);
        selected = joliet_status == NULL;
        if (image->malformed_directory) {
            snprintf(joliet_reason, sizeof(joliet_reason), "malformed directory near sector %llu",
                     (unsigned long long)(image->malformed_offset / SECTOR));
            joliet_status = joliet_reason;
        }
    }
    if (!selected && primary.valid && iso_end) {
        image->malformed_directory = false;
        image->joliet = false;
        image->root = primary.root;
        image->iso_blocks = primary.blocks;
        iso_status = check_layout(image);
        selected = iso_status == NULL;
        if (image->malformed_directory) {
            snprintf(iso_reason, sizeof(iso_reason), "malformed directory near sector %llu",
                     (unsigned long long)(image->malformed_offset / SECTOR));
            iso_status = iso_reason;
        }
    }
    char read_failure[64] = "";
    if (image->block_failure)
        snprintf(read_failure, sizeof(read_failure), ", block read failed at LBA %u", image->first_failed_lba);
    snprintf(diagnostic, capacity, "%u sectors. UDF (BEA01=%d NSR02=%d NSR03=%d TEA01=%d%s): %s. "
             "Joliet: %s. ISO9660 (terminator=%d): %s.%s%s%s",
             image->blocks, bea, nsr2, nsr3, tea, read_failure,
             udf_status ? udf_status : "Windows setup files found",
             joliet_status ? joliet_status : "Windows setup files found",
             iso_end, iso_status ? iso_status : "Windows setup files found",
             selected ? " Selected " : "", selected ? disc_filesystem(image) : "", selected ? "." : "");
    if (selected) return image;
failure:
    disc_close(image);
    return NULL;
}

void disc_close(DiscImage *image) {
    if (!image) return;
    if (image->udf) udfread_close(image->udf);
    if (image->fd >= 0) close(image->fd);
    free(image);
}
