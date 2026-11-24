/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "installer_disk.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <fatfs/ff.h>
#include <fatfs/diskio.h>
#include <udfread/udfread.h>

enum {
    SECTOR = 512,
    ALIGNMENT = 2048,
    BOOT_CLUSTER = 2048,
    PAYLOAD_CLUSTER = 4096,
    GPT_TABLE_SECTORS = 32,
    COPY_BUFFER = 512 << 10,
    MAX_DEPTH = 24,
    PATH_LENGTH = 1024
};

typedef struct InstallerPartition {
    uint64_t start;
    uint64_t sectors;
} InstallerPartition;

/* FatFs registers volumes globally. The two devices are leased for one build at a time. */
static pthread_mutex_t volume_lock = PTHREAD_MUTEX_INITIALIZER;
static InstallerPartition volumes[2];
static int image_fd = -1;
static bool image_readonly;

static int transfer(int fd, void *buffer, size_t length, uint64_t offset, bool write) {
    uint8_t *bytes = buffer;
    while (length) {
        ssize_t count = write ? pwrite(fd, bytes, length, (off_t)offset)
                              : pread(fd, bytes, length, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        bytes += count;
        offset += (uint64_t)count;
        length -= (size_t)count;
    }
    return 0;
}

DSTATUS disk_status(BYTE drive) {
    return drive < 2 && image_fd >= 0 ? (image_readonly ? STA_PROTECT : 0) : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE drive) { return disk_status(drive); }

static DRESULT disk_transfer(BYTE drive, void *buffer, LBA_t sector, UINT count, bool write) {
    DSTATUS status = disk_status(drive);
    if (status & STA_NOINIT) return RES_NOTRDY;
    if (write && (status & STA_PROTECT)) return RES_WRPRT;
    InstallerPartition part = volumes[drive];
    if (!count || sector >= part.sectors || count > part.sectors - sector) return RES_PARERR;
    return transfer(image_fd, buffer, (size_t)count * SECTOR, (part.start + sector) * SECTOR, write) == 0
        ? RES_OK : RES_ERROR;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    return disk_transfer(drive, buffer, sector, count, false);
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
    /* transfer does not mutate the buffer in its write branch. */
    return disk_transfer(drive, (void *)buffer, sector, count, true);
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
    if (disk_status(drive) & STA_NOINIT) return RES_NOTRDY;
    switch (command) {
    /* Writes go directly to a temporary file. The builder syncs it before publication. */
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buffer = volumes[drive].sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buffer = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buffer = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

static void write_le(uint8_t *field, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) field[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t gpt_crc(const uint8_t *bytes, size_t count) {
    uint32_t crc = UINT32_MAX;
    while (count--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

static void random_guid(uint8_t *guid) {
    arc4random_buf(guid, 16);
    guid[7] = (guid[7] & 0x0f) | 0x40;
    guid[8] = (guid[8] & 0x3f) | 0x80;
}

static void gpt_header(uint8_t *sector, uint64_t current, uint64_t backup, uint64_t entries,
                       uint64_t last_sector, const uint8_t *disk_guid, uint32_t entries_crc) {
    memset(sector, 0, SECTOR);
    memcpy(sector, "EFI PART", 8);
    write_le(sector + 8, 0x00010000, 4);
    write_le(sector + 12, 92, 4);
    write_le(sector + 24, current, 8);
    write_le(sector + 32, backup, 8);
    write_le(sector + 40, GPT_TABLE_SECTORS + 2, 8);
    write_le(sector + 48, last_sector - GPT_TABLE_SECTORS - 1, 8);
    memcpy(sector + 56, disk_guid, 16);
    write_le(sector + 72, entries, 8);
    write_le(sector + 80, 128, 4);
    write_le(sector + 84, 128, 4);
    write_le(sector + 88, entries_crc, 4);
    write_le(sector + 16, gpt_crc(sector, 92), 4);
}

static int write_gpt(uint64_t last_sector) {
    static const uint8_t esp_guid[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                         0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
    static const uint8_t basic_data_guid[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                                0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
    uint8_t mbr[SECTOR] = {0}, header[SECTOR], table[GPT_TABLE_SECTORS * SECTOR] = {0}, disk_guid[16];
    mbr[447] = 0x02;
    mbr[450] = 0xee;
    mbr[451] = mbr[452] = mbr[453] = 0xff;
    write_le(mbr + 454, 1, 4);
    write_le(mbr + 458, last_sector < UINT32_MAX ? last_sector : UINT32_MAX, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;

    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *entry = table + i * 128;
        const char *name = i ? "Windows Images" : "Windows PE";
        memcpy(entry, i ? basic_data_guid : esp_guid, 16);
        random_guid(entry + 16);
        write_le(entry + 32, volumes[i].start, 8);
        write_le(entry + 40, volumes[i].start + volumes[i].sectors - 1, 8);
        for (size_t n = 0; name[n]; ++n) entry[56 + n * 2] = (uint8_t)name[n];
    }

    random_guid(disk_guid);
    uint32_t checksum = gpt_crc(table, sizeof(table));
    uint64_t backup_entries = last_sector - GPT_TABLE_SECTORS;
    gpt_header(header, 1, last_sector, 2, last_sector, disk_guid, checksum);
    if (transfer(image_fd, mbr, SECTOR, 0, true) ||
        transfer(image_fd, header, SECTOR, SECTOR, true) ||
        transfer(image_fd, table, sizeof(table), 2 * SECTOR, true)) return -1;
    gpt_header(header, last_sector, 1, backup_entries, last_sector, disk_guid, checksum);
    return transfer(image_fd, table, sizeof(table), backup_entries * SECTOR, true) ||
           transfer(image_fd, header, SECTOR, last_sector * SECTOR, true) ? -1 : 0;
}

typedef struct InstallerBuild {
    uint64_t planned[2];
    uint64_t total;
    uint64_t copied;
    uint64_t iso_size;
    unsigned payloads;
    bool boot_loader;
    bool boot_wim;
    bool boot_bcd;
    int boot_wim_fd;
    uint64_t boot_wim_size;
    uint8_t *buffer;
    WinIOSInstallerProgress progress;
    void *context;
    char *error;
    size_t error_size;
} InstallerBuild;

static int fail(InstallerBuild *build, const char *message) {
    if (build->error && build->error_size) snprintf(build->error, build->error_size, "%s", message);
    return -1;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static int note_progress(InstallerBuild *build) {
    if (build->progress && build->progress(build->copied, build->total, build->context))
        return fail(build, "Installer disk preparation was cancelled.");
    return 0;
}

static int stream_file(InstallerBuild *build, UDFFILE *input, int input_fd, const char *path, uint64_t length) {
    FIL output;
    if (f_open(&output, path, FA_CREATE_NEW | FA_WRITE) != FR_OK)
        return fail(build, "An installer file could not be created. Check for duplicate or invalid media filenames.");
    int result = 0;
    uint64_t offset = 0;
    while (length) {
        UINT piece = length < COPY_BUFFER ? (UINT)length : COPY_BUFFER;
        UINT written = 0;
        if (input ? udfread_file_read(input, build->buffer, piece) != (ssize_t)piece
                  : transfer(input_fd, build->buffer, piece, offset, false) != 0) {
            result = fail(build, "A file inside the installation ISO is incomplete.");
            break;
        }
        if (f_write(&output, build->buffer, piece, &written) != FR_OK || written != piece) {
            result = fail(build, "An installer file could not be written. Check available device storage.");
            break;
        }
        length -= piece;
        offset += piece;
        build->copied += piece;
        if (note_progress(build)) {
            result = -1;
            break;
        }
    }
    if (f_close(&output) != FR_OK && !result)
        return fail(build, "An installer file could not be saved.");
    return result;
}

static bool safe_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length > 255 || name[length - 1] == '.' || name[length - 1] == ' ') return false;
    for (const uint8_t *p = (const uint8_t *)name; *p; ++p) {
        if (*p < 32 || *p == 0x7f || (*p == 0xc0 && p[1] == 0x80) || strchr("\\/:*?\"<>|", *p))
            return false;
    }
    return true;
}

static int walk(InstallerBuild *build, UDFDIR *dir, const char *prefix, int depth, bool copying) {
    if (depth >= MAX_DEPTH) return fail(build, "An installer directory is nested too deeply.");
    struct udfread_dirent entry;
    while (udfread_readdir(dir, &entry)) {
        if (!entry.d_name) continue;
        if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, "..")) continue;
        if (!safe_name(entry.d_name)) return fail(build, "The ISO contains an unsupported filename.");
        char path[PATH_LENGTH];
        int count = snprintf(path, sizeof(path), "0:%s/%s", prefix, entry.d_name);
        if (count < 0 || (size_t)count >= sizeof(path)) return fail(build, "An installer path is too long.");
        if (entry.d_type == UDF_DT_DIR) {
            if (copying && f_mkdir(path) != FR_OK) return fail(build, "An installer folder could not be created.");
            UDFDIR *child = udfread_opendir_at(dir, entry.d_name);
            if (!child) return fail(build, "An installer folder could not be read.");
            int result = walk(build, child, path + 2, depth + 1, copying);
            udfread_closedir(child);
            if (result) return result;
            continue;
        }
        if (entry.d_type != UDF_DT_REG) return fail(build, "The ISO contains an unsupported file type.");
        bool payload = !strcasecmp(prefix, "/sources") &&
            (!strcasecmp(entry.d_name, "install.wim") || !strcasecmp(entry.d_name, "install.esd"));
        UDFFILE *file = udfread_file_openat(dir, entry.d_name);
        if (!file) return fail(build, "An installer file could not be opened.");
        bool is_boot_wim = !strcasecmp(path + 2, "/sources/boot.wim");
        bool use_prepared_wim = is_boot_wim && build->boot_wim_fd >= 0;
        int64_t length = use_prepared_wim ? (int64_t)build->boot_wim_size : udfread_file_size(file);
        int result = 0;
        if (length < 0 || (!use_prepared_wim && (uint64_t)length > build->iso_size)) {
            result = fail(build, "An installer file size exceeds the ISO.");
        } else if (!payload && (uint64_t)length > UINT32_MAX) {
            result = fail(build, "A file needed on the FAT32 boot partition exceeds 4 GiB.");
        } else if (copying && (uint64_t)length > build->total - build->copied) {
            result = fail(build, "The source changed during installer preparation.");
        } else if (copying) {
            if (payload) path[0] = '1';
            result = stream_file(build, use_prepared_wim ? NULL : file, build->boot_wim_fd, path, (uint64_t)length);
        } else if ((uint64_t)length > 2 * build->iso_size - build->total ||
                   align_up((uint64_t)length, PAYLOAD_CLUSTER) >
                       (uint64_t)INT64_MAX / 2 - build->planned[0] - build->planned[1]) {
            result = fail(build, "The combined installer files are too large for this ISO.");
        } else {
            build->planned[payload] += align_up((uint64_t)length, payload ? PAYLOAD_CLUSTER : BOOT_CLUSTER);
            build->total += (uint64_t)length;
            if (payload) ++build->payloads;
            if (!strcasecmp(path + 2, "/efi/boot/bootaa64.efi")) build->boot_loader = length > 0;
            if (is_boot_wim) build->boot_wim = length > 0;
            if (!strcasecmp(path + 2, "/efi/microsoft/boot/bcd") ||
                !strcasecmp(path + 2, "/boot/bcd")) build->boot_bcd = length > 0;
        }
        udfread_file_close(file);
        if (result) return result;
    }
    return 0;
}

static int stage_drivers(InstallerBuild *build, const char *directory, bool copying) {
    static const char *const names[] = {"netkvm.inf", "netkvm.cat", "netkvm.sys", "netkvmco.dll"};
    if (!directory) return 0;
    if (copying && (f_mkdir("1:/drivers") != FR_OK || f_mkdir("1:/drivers/NetKVM") != FR_OK))
        return fail(build, "The NetKVM driver folder could not be created.");
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[PATH_LENGTH];
        int length = snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) return fail(build, "The NetKVM file path is too long.");
        int file = open(path, O_RDONLY | O_NONBLOCK);
        if (file < 0 && errno == ENOENT && i == 3) continue;
        struct stat st;
        if (file < 0 || fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            (uint64_t)st.st_size > UINT32_MAX) {
            if (file >= 0) close(file);
            return fail(build, "The NetKVM driver package is incomplete or invalid.");
        }
        int result = 0;
        if (copying) {
            snprintf(path, sizeof(path), "1:/drivers/NetKVM/%s", names[i]);
            if ((uint64_t)st.st_size > build->total - build->copied)
                result = fail(build, "The driver package changed during preparation.");
            else
                result = stream_file(build, NULL, file, path, (uint64_t)st.st_size);
        } else {
            build->planned[1] += align_up((uint64_t)st.st_size, PAYLOAD_CLUSTER);
            build->total += (uint64_t)st.st_size;
        }
        close(file);
        if (result) return result;
    }
    return 0;
}

static int format_partitions(InstallerBuild *build, FATFS *boot, FATFS *payload) {
    MKFS_PARM fat32 = {.fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = BOOT_CLUSTER};
    MKFS_PARM exfat = {.fmt = FM_EXFAT | FM_SFD, .au_size = PAYLOAD_CLUSTER};
    if (f_mkfs("0:", &fat32, build->buffer, COPY_BUFFER) != FR_OK ||
        f_mkfs("1:", &exfat, build->buffer, COPY_BUFFER) != FR_OK ||
        f_mount(boot, "0:", 1) != FR_OK || f_mount(payload, "1:", 1) != FR_OK ||
        f_mkdir("1:/sources") != FR_OK)
        return fail(build, "The FAT32 boot partition or exFAT payload partition could not be created.");
    FIL marker;
    if (f_open(&marker, "1:/WINIOS.MEDIA", FA_CREATE_NEW | FA_WRITE) != FR_OK || f_close(&marker) != FR_OK)
        return fail(build, "The installer payload marker could not be written.");
    return 0;
}

int winios_build_installer_disk(const char *iso_path, const char *output_path, const char *boot_wim_path,
                                const char *driver_directory,
                                WinIOSInstallerProgress progress, void *context,
                                char *error, size_t error_size) {
    InstallerBuild build = {.boot_wim_fd = -1, .progress = progress, .context = context, .error = error, .error_size = error_size};
    struct stat info;
    udfread *udf = NULL;
    UDFDIR *root = NULL;
    FATFS boot, payload;
    char *staging = NULL;
    int result = -1;
    if (!iso_path || !output_path || stat(iso_path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0)
        return fail(&build, "The selected ISO is not a readable file.");

    build.iso_size = (uint64_t)info.st_size;
    pthread_mutex_lock(&volume_lock);
    if (boot_wim_path) {
        build.boot_wim_fd = open(boot_wim_path, O_RDONLY | O_NONBLOCK);
        if (build.boot_wim_fd < 0 || fstat(build.boot_wim_fd, &info) || !S_ISREG(info.st_mode) ||
            info.st_size <= 0 || info.st_size > UINT32_MAX) {
            fail(&build, "The staged Windows PE boot image is invalid.");
            goto done;
        }
        build.boot_wim_size = (uint64_t)info.st_size;
    }
    udf = udfread_init();
    if (!udf || udfread_open(udf, iso_path) != 0 || !(root = udfread_opendir(udf, "/"))) {
        fail(&build, "The installation ISO has no readable UDF filesystem.");
        goto done;
    }
    if (walk(&build, root, "", 0, false) || stage_drivers(&build, driver_directory, false)) goto done;
    if (build.payloads != 1 || !build.boot_loader || !build.boot_wim || !build.boot_bcd) {
        fail(&build, "The ISO needs an ARM64 EFI loader, boot configuration, WinPE and exactly one install image.");
        goto done;
    }
    if (note_progress(&build)) goto done;

    size_t name_size = strlen(output_path) + sizeof(".building.XXXXXX");
    staging = malloc(name_size);
    build.buffer = malloc(COPY_BUFFER);
    if (!staging || !build.buffer) {
        fail(&build, "There is not enough memory to prepare the installer disk.");
        goto done;
    }
    snprintf(staging, name_size, "%s.building.XXXXXX", output_path);
    image_fd = mkstemp(staging);
    if (image_fd < 0) {
        fail(&build, "The installer’s temporary file could not be created.");
        goto done;
    }

    struct statvfs storage;
    if (fstatvfs(image_fd, &storage) == 0 && storage.f_frsize &&
        storage.f_bavail < (build.planned[0] + build.planned[1] + (128ull << 20)) / storage.f_frsize) {
        fail(&build, "There is not enough free storage for a temporary copy of the installer files.");
        goto done;
    }
    uint64_t boot_bytes = build.planned[0] + (64ull << 20) + build.planned[0] / 32;
    if (boot_bytes < (256ull << 20)) boot_bytes = 256ull << 20;
    uint64_t payload_bytes = build.planned[1] + (32ull << 20) + build.planned[1] / 32;
    volumes[0] = (InstallerPartition){ALIGNMENT, align_up(boot_bytes, ALIGNMENT * SECTOR) / SECTOR};
    volumes[1] = (InstallerPartition){volumes[0].start + volumes[0].sectors,
                                      align_up(payload_bytes, ALIGNMENT * SECTOR) / SECTOR};
    uint64_t last_sector = volumes[1].start + volumes[1].sectors + GPT_TABLE_SECTORS;
    if (last_sector >= INT64_MAX / SECTOR || ftruncate(image_fd, (off_t)((last_sector + 1) * SECTOR)) ||
        write_gpt(last_sector)) {
        fail(&build, "The sparse GPT installer disk could not be written.");
        goto done;
    }
    if (format_partitions(&build, &boot, &payload)) goto done;
    udfread_rewinddir(root);
    if (walk(&build, root, "", 0, true) || stage_drivers(&build, driver_directory, true)) goto done;
    if (build.copied != build.total || fsync(image_fd)) {
        fail(&build, "The installer disk could not be synced or the source changed during preparation.");
        goto done;
    }
    if (link(staging, output_path) != 0) {
        fail(&build, "The installer disk could not be published. An existing disk was preserved.");
        goto done;
    }
    result = 0;

done:
    if (image_fd >= 0) {
        f_mount(NULL, "0:", 0);
        f_mount(NULL, "1:", 0);
        close(image_fd);
        image_fd = -1;
        if (staging) unlink(staging);
    }
    if (build.boot_wim_fd >= 0) close(build.boot_wim_fd);
    if (root) udfread_closedir(root);
    if (udf) udfread_close(udf);
    free(staging);
    free(build.buffer);
    pthread_mutex_unlock(&volume_lock);
    return result;
}


static bool is_arm64_efi(FIL *file) {
    uint8_t dos[64], header[6];
    UINT bytes;
    if (f_read(file, dos, sizeof(dos), &bytes) != FR_OK || bytes != sizeof(dos) ||
        dos[0] != 'M' || dos[1] != 'Z') return false;
    DWORD offset = 0;
    for (unsigned i = 0; i < 4; ++i) offset |= (DWORD)dos[60 + i] << (8 * i);
    return offset >= sizeof(dos) && offset < f_size(file) &&
           f_lseek(file, offset) == FR_OK && f_read(file, header, sizeof(header), &bytes) == FR_OK &&
           bytes == sizeof(header) && memcmp(header, "PE\0\0\x64\xaa", sizeof(header)) == 0;
}

int winios_has_windows_boot_files(const char *disk_path, uint64_t start, uint64_t sectors) {
    if (!disk_path || !sectors) return 0;
    pthread_mutex_lock(&volume_lock);
    image_fd = open(disk_path, O_RDONLY | O_NONBLOCK);
    image_readonly = true;
    struct stat info;
    FATFS firmware;
    FIL loader;
    FILINFO bcd;
    bool found = false;
    if (image_fd >= 0 && !fstat(image_fd, &info) && S_ISREG(info.st_mode)) {
        uint64_t limit = (uint64_t)info.st_size / SECTOR;
        if (start < limit && sectors <= limit - start) {
            volumes[0] = (InstallerPartition){start, sectors};
            if (f_mount(&firmware, "0:", 1) == FR_OK &&
                f_stat("0:/EFI/Microsoft/Boot/BCD", &bcd) == FR_OK && bcd.fsize > 0 &&
                f_open(&loader, "0:/EFI/Microsoft/Boot/bootmgfw.efi", FA_READ) == FR_OK) {
                found = is_arm64_efi(&loader);
                f_close(&loader);
            }
        }
    }
    f_mount(NULL, "0:", 0);
    if (image_fd >= 0) close(image_fd);
    image_fd = -1;
    image_readonly = false;
    pthread_mutex_unlock(&volume_lock);
    return found;
}
