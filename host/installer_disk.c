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
#include "disc_image.h"

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

static const uint8_t esp_guid[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t basic_data_guid[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                            0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
static const uint8_t msr_guid[16] = {0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d,
                                     0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae};

typedef struct PartitionSpec {
    InstallerPartition range;
    const uint8_t *type;
    const char *name;
} PartitionSpec;

static int write_gpt(int fd, uint64_t last_sector, const PartitionSpec *parts, unsigned count,
                     const uint8_t disk_guid[16]) {
    uint8_t mbr[SECTOR] = {0}, header[SECTOR], table[GPT_TABLE_SECTORS * SECTOR] = {0};
    mbr[447] = 0x02;
    mbr[450] = 0xee;
    mbr[451] = mbr[452] = mbr[453] = 0xff;
    write_le(mbr + 454, 1, 4);
    write_le(mbr + 458, last_sector < UINT32_MAX ? last_sector : UINT32_MAX, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;

    for (unsigned i = 0; i < count; ++i) {
        uint8_t *entry = table + i * 128;
        const char *name = parts[i].name;
        memcpy(entry, parts[i].type, 16);
        random_guid(entry + 16);
        write_le(entry + 32, parts[i].range.start, 8);
        write_le(entry + 40, parts[i].range.start + parts[i].range.sectors - 1, 8);
        for (size_t n = 0; name[n]; ++n) entry[56 + n * 2] = (uint8_t)name[n];
    }

    uint32_t checksum = gpt_crc(table, sizeof(table));
    uint64_t backup_entries = last_sector - GPT_TABLE_SECTORS;
    gpt_header(header, 1, last_sector, 2, last_sector, disk_guid, checksum);
    if (transfer(fd, mbr, SECTOR, 0, true) ||
        transfer(fd, header, SECTOR, SECTOR, true) ||
        transfer(fd, table, sizeof(table), 2 * SECTOR, true)) return -1;
    gpt_header(header, last_sector, 1, backup_entries, last_sector, disk_guid, checksum);
    return transfer(fd, table, sizeof(table), backup_entries * SECTOR, true) ||
           transfer(fd, header, SECTOR, last_sector * SECTOR, true) ? -1 : 0;
}

static int write_media_gpt(uint64_t last_sector, bool logs_only) {
    uint8_t guid[16];
    random_guid(guid);
    PartitionSpec parts[] = {
        {volumes[0], logs_only ? basic_data_guid : esp_guid, logs_only ? "Setup Diagnostics" : "Windows PE"},
        {volumes[1], basic_data_guid, "Windows Images"}
    };
    return write_gpt(image_fd, last_sector, parts, logs_only ? 1 : 2, guid);
}

static int hex_digit(char c) {
    const char *digits = "0123456789abcdef";
    if (c >= 'A' && c <= 'F') c += 'a' - 'A';
    const char *match = strchr(digits, c);
    return match ? (int)(match - digits) : -1;
}

int winios_gpt_guid(const char *text, uint8_t uuid[16]) {
    static const uint8_t gpt_order[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    if (!text || strlen(text) != 36) return -1;
    for (unsigned i = 0; i < 16; i++) {
        if ((i == 4 || i == 6 || i == 8 || i == 10) && *text++ != '-') return -1;
        int hi = hex_digit(*text++), lo = hex_digit(*text++);
        if (hi < 0 || lo < 0) return -1;
        uuid[gpt_order[i]] = (uint8_t)(hi * 16 + lo);
    }
    return 0;
}

static void direct_layout(PartitionSpec parts[3], uint64_t sectors) {
    const uint64_t esp = (100u << 20) / SECTOR, msr = (16u << 20) / SECTOR;
    uint64_t windows = ALIGNMENT + esp + msr;
    /* Leave the backup GPT in a separate allocation block from the Windows partition. */
    parts[0] = (PartitionSpec){{ALIGNMENT, esp}, esp_guid, "System"};
    parts[1] = (PartitionSpec){{ALIGNMENT + esp, msr}, msr_guid, "Microsoft reserved"};
    parts[2] = (PartitionSpec){{windows, sectors - ALIGNMENT - windows}, basic_data_guid, "Windows"};
}

int winios_create_direct_target(const char *path, uint64_t bytes, const char *target_guid) {
    uint8_t guid[16];
    if (!path || bytes < (64ull << 30) || bytes > INT64_MAX || bytes % (ALIGNMENT * SECTOR) ||
        winios_gpt_guid(target_guid, guid)) return -1;
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    PartitionSpec parts[3];
    direct_layout(parts, bytes / SECTOR);
    int failed = ftruncate(fd, (off_t)bytes) || write_gpt(fd, bytes / SECTOR - 1, parts, 3, guid) || fsync(fd);
    if (close(fd)) failed = 1;
    if (failed) unlink(path);
    return failed ? -1 : 0;
}

typedef struct InstallerBuild {
    uint64_t planned[2];
    uint64_t total;
    uint64_t copied;
    uint64_t iso_size;
    unsigned payloads;
    bool boot_loader;
    bool boot_wim;
    bool setup_program;
    bool boot_bcd;
    const char *direct_guid;
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

static int stream_file(InstallerBuild *build, DiscFile *input, int input_fd, const char *path, uint64_t length) {
    FIL output;
    if (f_open(&output, path, FA_CREATE_NEW | FA_WRITE) != FR_OK)
        return fail(build, "An installer file could not be created. Check for duplicate or invalid media filenames.");
    int result = 0;
    uint64_t offset = 0;
    while (length) {
        UINT piece = length < COPY_BUFFER ? (UINT)length : COPY_BUFFER;
        UINT written = 0;
        if (input ? disc_file_pread(input, build->buffer, piece, offset) != (ssize_t)piece
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

/* WinPE boots from FAT32. In the modern layout Windows Setup runs from the
 * complete exFAT source tree, with the already loaded boot.wim kept on FAT32. */
static unsigned installer_destinations(const InstallerBuild *build, const char *path, bool image, bool boot_wim) {
    if (boot_wim) return 1;
    if (build->boot_wim_fd < 0) return image ? 2 : 1;
    if (!strncasecmp(path, "/boot/", 6) || !strncasecmp(path, "/efi/", 5) ||
        !strcasecmp(path, "/bootmgr") || !strcasecmp(path, "/bootmgr.efi"))
        return build->direct_guid ? 1 : 3;
    if (build->direct_guid && !image) return 0;
    return 2;
}

static bool installer_setup_is_arm64(DiscFile *file) {
    uint8_t dos[64], pe[6];
    if (disc_file_pread(file, dos, sizeof dos, 0) != sizeof dos || memcmp(dos, "MZ", 2)) return false;
    uint32_t offset = 0;
    for (unsigned i = 0; i < 4; ++i) offset |= (uint32_t)dos[60 + i] << (8 * i);
    return offset >= sizeof dos && disc_file_pread(file, pe, sizeof pe, offset) == sizeof pe &&
           !memcmp(pe, "PE\0\0\x64\xaa", sizeof pe);
}

static int walk(InstallerBuild *build, DiscDirectory *dir, const char *prefix, int depth, bool copying) {
    if (depth >= MAX_DEPTH) return fail(build, "An installer directory is nested too deeply.");
    DiscEntry entry;
    int status;
    while ((status = disc_readdir(dir, &entry)) == 1) {
        if (!entry.name) continue;
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
        if (!safe_name(entry.name)) return fail(build, "The ISO contains an unsupported filename.");
        char path[PATH_LENGTH];
        int count = snprintf(path, sizeof(path), "0:%s/%s", prefix, entry.name);
        if (count < 0 || (size_t)count >= sizeof(path)) return fail(build, "An installer path is too long.");
        if (entry.type == DISC_DIR) {
            if (build->direct_guid && depth == 0 && strcasecmp(entry.name, "boot") &&
                strcasecmp(entry.name, "efi") && strcasecmp(entry.name, "sources")) continue;
            if (copying) {
                if (f_mkdir(path) != FR_OK) return fail(build, "An installer folder could not be created.");
                if (build->boot_wim_fd >= 0 && !build->direct_guid) {
                    path[0] = '1';
                    FRESULT folder = f_mkdir(path);
                    if (folder != FR_OK && (folder != FR_EXIST || strcasecmp(path + 2, "/sources")))
                        return fail(build, "A Setup source folder could not be created on the payload volume.");
                    path[0] = '0';
                }
            }
            DiscDirectory *child = disc_opendir_at(dir, entry.name);
            if (!child) return fail(build, "An installer folder could not be read.");
            int result = walk(build, child, path + 2, depth + 1, copying);
            disc_closedir(child);
            if (result) return result;
            continue;
        }
        if (entry.type != DISC_REG) return fail(build, "The ISO contains an unsupported file type.");
        bool payload = !strcasecmp(prefix, "/sources") &&
            (!strcasecmp(entry.name, "install.wim") || !strcasecmp(entry.name, "install.esd"));
        DiscFile *file = disc_open_at(dir, entry.name);
        if (!file) return fail(build, "An installer file could not be opened.");
        bool is_boot_wim = !strcasecmp(path + 2, "/sources/boot.wim");
        bool use_prepared_wim = is_boot_wim && build->boot_wim_fd >= 0;
        int64_t length = use_prepared_wim ? (int64_t)build->boot_wim_size : disc_file_size(file);
        int result = 0;
        unsigned destinations = installer_destinations(build, path + 2, payload, is_boot_wim);
        if (length < 0 || (!use_prepared_wim && (uint64_t)length > build->iso_size)) {
            result = fail(build, "An installer file size exceeds the ISO.");
        } else if ((destinations & 1u) && (uint64_t)length > UINT32_MAX) {
            result = fail(build, "A file needed on the FAT32 boot partition exceeds 4 GiB.");
        } else {
            for (unsigned i = 0; i < 2 && !result; ++i) {
                if (!(destinations & (1u << i))) continue;
                if (copying) {
                    if ((uint64_t)length > build->total - build->copied) {
                        result = fail(build, "The source changed during installer preparation.");
                        break;
                    }
                    path[0] = '0' + (char)i;
                    result = stream_file(build, use_prepared_wim ? NULL : file, build->boot_wim_fd, path, (uint64_t)length);
                } else if ((uint64_t)length > 2 * build->iso_size - build->total ||
                           align_up((uint64_t)length, PAYLOAD_CLUSTER) >
                               (uint64_t)INT64_MAX / 2 - build->planned[0] - build->planned[1]) {
                    result = fail(build, "The combined installer files are too large for this ISO.");
                } else {
                    build->planned[i] += align_up((uint64_t)length, i ? PAYLOAD_CLUSTER : BOOT_CLUSTER);
                    build->total += (uint64_t)length;
                }
            }
            if (!copying) {
                if (payload) ++build->payloads;
                if (!strcasecmp(path + 2, "/efi/boot/bootaa64.efi")) build->boot_loader = length > 0;
                if (is_boot_wim) build->boot_wim = length > 0;
                if (!strcasecmp(path + 2, "/efi/microsoft/boot/bcd") ||
                    !strcasecmp(path + 2, "/boot/bcd")) build->boot_bcd = length > 0;
                if (!strcasecmp(path + 2, "/sources/setup.exe")) build->setup_program = installer_setup_is_arm64(file);
            }
        }
        disc_file_close(file);
        if (result) return result;
    }
    return status < 0 ? fail(build, "A directory in the ISO is malformed or incomplete.") : 0;
}

static int stage_drivers(InstallerBuild *build, const char *directory, bool copying) {
    static const char *const names[] = {
        "netkvm.inf", "netkvm.cat", "netkvm.sys",
        "netkvmp.exe", "netkvmco.dll", "netkvmco.exe"
    };
    if (!directory) return 0;
    if (copying && (f_mkdir("1:/drivers") != FR_OK || f_mkdir("1:/drivers/NetKVM") != FR_OK))
        return fail(build, "The NetKVM driver folder could not be created.");
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[PATH_LENGTH];
        int length = snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) return fail(build, "The NetKVM file path is too long.");
        int file = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
        if (file < 0 && errno == ENOENT && i >= 3) continue;
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

static int stage_answer(InstallerBuild *build, const char *answer, bool copying) {
    if (!answer) return 0;
    int file = open(answer, O_RDONLY | O_NONBLOCK);
    struct stat st;
    if (file < 0 || fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 64 << 10) {
        if (file >= 0) close(file);
        return fail(build, "The Windows Setup answer file is invalid.");
    }
    int result = 0;
    if (copying) {
        if ((uint64_t)st.st_size > build->total - build->copied)
            result = fail(build, "The Setup answer file changed during preparation.");
        else
            result = stream_file(build, NULL, file, "1:/WINIOS-SETUP.XML", (uint64_t)st.st_size);
    } else {
        build->planned[1] += align_up((uint64_t)st.st_size, PAYLOAD_CLUSTER);
        build->total += (uint64_t)st.st_size;
    }
    close(file);
    return result;
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
    if (build->direct_guid) {
        UINT written = 0;
        if (f_open(&marker, "1:/WINIOS.DIRECT", FA_CREATE_NEW | FA_WRITE) != FR_OK)
            return fail(build, "The DISM target identity could not be staged.");
        char record[39];
        snprintf(record, sizeof(record), "%s\r\n", build->direct_guid);
        FRESULT saved = f_write(&marker, record, 38, &written);
        if (f_close(&marker) != FR_OK || saved != FR_OK || written != 38)
            return fail(build, "The DISM target identity could not be written.");
    } else if (build->boot_wim_fd >= 0) {
        if (f_open(&marker, "1:/WINIOS.V2", FA_CREATE_NEW | FA_WRITE) != FR_OK || f_close(&marker) != FR_OK)
            return fail(build, "The Setup layout marker could not be written.");
    }
    return 0;
}

static int build_installer_disk(const char *iso_path, const char *output_path, const char *boot_wim_path,
                                const char *driver_directory, const char *answer_path, const char *direct_guid,
                                WinIOSInstallerProgress progress, void *context, char *error, size_t error_size) {
    InstallerBuild build = {.boot_wim_fd = -1, .direct_guid = direct_guid, .progress = progress,
                            .context = context, .error = error, .error_size = error_size};
    struct stat info;
    DiscImage *image = NULL;
    DiscDirectory *root = NULL;
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
    image = disc_open_windows(iso_path, error, error_size);
    if (!image) goto done;
    root = disc_root(image);
    if (!root) {
        fail(&build, "The installation ISO root directory could not be opened.");
        goto done;
    }
    if (walk(&build, root, "", 0, false) || stage_drivers(&build, driver_directory, false) ||
        stage_answer(&build, answer_path, false)) goto done;
    if (build.payloads != 1 || !build.boot_loader || !build.boot_wim || !build.boot_bcd) {
        fail(&build, "The ISO needs an ARM64 EFI loader, boot configuration, WinPE and exactly one install image.");
        goto done;
    }
    if (build.boot_wim_fd >= 0 && !direct_guid && !build.setup_program) {
        fail(&build, "The ISO payload is missing a valid ARM64 sources/setup.exe.");
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
        write_media_gpt(last_sector, false)) {
        fail(&build, "The sparse GPT installer disk could not be written.");
        goto done;
    }
    if (format_partitions(&build, &boot, &payload)) goto done;
    disc_rewinddir(root);
    if (walk(&build, root, "", 0, true) || stage_drivers(&build, driver_directory, true) ||
        stage_answer(&build, answer_path, true)) goto done;
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
    if (root) disc_closedir(root);
    disc_close(image);
    free(staging);
    free(build.buffer);
    pthread_mutex_unlock(&volume_lock);
    return result;
}

int winios_build_installer_disk(const char *iso_path, const char *output_path, const char *boot_wim_path,
                                const char *driver_directory, const char *answer_path,
                                WinIOSInstallerProgress progress, void *context, char *error, size_t error_size) {
    return build_installer_disk(iso_path, output_path, boot_wim_path, driver_directory, answer_path,
                                NULL, progress, context, error, error_size);
}

int winios_build_direct_installer_disk(const char *iso_path, const char *output_path, const char *boot_wim_path,
                                      const char *driver_directory, const char *target_guid,
                                      WinIOSInstallerProgress progress, void *context, char *error, size_t error_size) {
    uint8_t guid[16];
    if (!boot_wim_path || winios_gpt_guid(target_guid, guid)) {
        if (error && error_size) snprintf(error, error_size, "The DISM installation plan is invalid.");
        return -1;
    }
    return build_installer_disk(iso_path, output_path, boot_wim_path, driver_directory, NULL,
                                target_guid, progress, context, error, error_size);
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


static uint64_t field_le(const uint8_t *field, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)field[i] << (i * 8);
    return value;
}

static bool blank_target(const char *path) {
    if (!path) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    bool blank = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= (64ll << 30) &&
                 lseek(fd, 0, SEEK_DATA) < 0 && errno == ENXIO;
    close(fd);
    return blank;
}

static bool direct_target_is_untouched(const char *path, const uint8_t guid[16]) {
    if (!path) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    bool valid = false;
    struct stat info;
    uint8_t header[SECTOR], table[GPT_TABLE_SECTORS * SECTOR];
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < (64ll << 30) ||
        info.st_size % (ALIGNMENT * SECTOR) || transfer(fd, header, sizeof(header), SECTOR, false) ||
        memcmp(header, "EFI PART", 8) || memcmp(header + 56, guid, 16) || field_le(header + 72, 8) != 2 ||
        transfer(fd, table, sizeof(table), 2 * SECTOR, false)) goto done;
    PartitionSpec parts[3];
    uint64_t sectors = (uint64_t)info.st_size / SECTOR;
    uint8_t expected[SECTOR], backup[sizeof(table)];
    uint32_t crc = gpt_crc(table, sizeof(table));
    gpt_header(expected, 1, sectors - 1, 2, sectors - 1, guid, crc);
    if (memcmp(header, expected, sizeof(header))) goto done;
    uint64_t backup_start = sectors - 1 - GPT_TABLE_SECTORS;
    gpt_header(expected, sectors - 1, 1, backup_start, sectors - 1, guid, crc);
    if (transfer(fd, header, sizeof(header), (sectors - 1) * SECTOR, false) || memcmp(header, expected, sizeof(header)) ||
        transfer(fd, backup, sizeof(backup), backup_start * SECTOR, false) || memcmp(table, backup, sizeof(table))) goto done;
    direct_layout(parts, sectors);
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t *entry = table + i * 128;
        if (memcmp(entry, parts[i].type, 16) || field_le(entry + 32, 8) != parts[i].range.start ||
            field_le(entry + 40, 8) != parts[i].range.start + parts[i].range.sectors - 1) goto done;
    }
    for (size_t i = 3 * 128; i < sizeof(table); i++) if (table[i]) goto done;
    off_t data = lseek(fd, (off_t)ALIGNMENT * SECTOR, SEEK_DATA);
    valid = data >= info.st_size - (off_t)ALIGNMENT * SECTOR || (data < 0 && errno == ENXIO);
done:
    close(fd);
    return valid;
}

int winios_configure_installer_answer(const char *installer, const char *primary) {
    if (!installer) return -1;
    pthread_mutex_lock(&volume_lock);
    image_readonly = true;
    image_fd = open(installer, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat info;
    FATFS payload;
    FIL marker;
    FILINFO answer;
    int result = -1;
    uint8_t header[SECTOR], table[GPT_TABLE_SECTORS * SECTOR];
    if (image_fd < 0 || fstat(image_fd, &info) || !S_ISREG(info.st_mode) ||
        info.st_size < (256ll << 20) ||
        transfer(image_fd, header, sizeof(header), SECTOR, false) ||
        memcmp(header, "EFI PART", 8) || field_le(header + 12, 4) != 92 ||
        field_le(header + 72, 8) != 2 || field_le(header + 80, 4) != 128 ||
        field_le(header + 84, 4) != 128 ||
        transfer(image_fd, table, sizeof(table), 2 * SECTOR, false) ||
        gpt_crc(table, sizeof(table)) != field_le(header + 88, 4)) goto done;
    uint32_t header_crc = (uint32_t)field_le(header + 16, 4);
    memset(header + 16, 0, 4);
    if (header_crc != gpt_crc(header, 92)) goto done;
    uint64_t start = field_le(table + 160, 8);
    uint64_t last = field_le(table + 168, 8);
    uint64_t boot_end = field_le(table + 40, 8);
    uint64_t sectors = (uint64_t)info.st_size / SECTOR;
    if (start <= boot_end || start >= sectors || last < start || last >= sectors) goto done;
    volumes[1] = (InstallerPartition){start, last - start + 1};
    if (f_mount(&payload, "1:", 1) != FR_OK || f_stat("1:/WINIOS.MEDIA", &answer) != FR_OK) goto done;
    FRESULT direct = f_stat("1:/WINIOS.DIRECT", &answer);
    if (direct == FR_OK) {
        result = -2;
        if (answer.fsize != 38 || f_open(&marker, "1:/WINIOS.DIRECT", FA_READ) != FR_OK) goto done;
        char text[38] = {0};
        uint8_t guid[16];
        UINT count = 0;
        FRESULT read = f_read(&marker, text, sizeof(text), &count);
        f_close(&marker);
        bool newline = text[36] == '\r' && text[37] == '\n';
        text[36] = '\0';
        if (read == FR_OK && count == sizeof(text) && newline && !winios_gpt_guid(text, guid) &&
            direct_target_is_untouched(primary, guid))
            result = 0;
        goto done;
    }
    if (direct != FR_NO_FILE) goto done;
    int writable = open(installer, O_RDWR | O_NOFOLLOW | O_NONBLOCK);
    struct stat same;
    if (writable < 0) goto done;
    if (fstat(writable, &same) || same.st_dev != info.st_dev || same.st_ino != info.st_ino || same.st_size != info.st_size) {
        close(writable);
        goto done;
    }
    close(image_fd);
    image_fd = writable;
    image_readonly = false;
    bool allow = blank_target(primary);
    if (allow && f_stat("1:/WINIOS.V2", &answer) == FR_OK &&
        f_stat("1:/WINIOS-SETUP.XML", &answer) == FR_OK && answer.fsize > 0) {
        if (f_open(&marker, "1:/WINIOS.AUTO", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK ||
            f_close(&marker) != FR_OK) goto done;
    } else {
        FRESULT removed = f_unlink("1:/WINIOS.AUTO");
        if (removed != FR_OK && removed != FR_NO_FILE) goto done;
    }
    result = fsync(image_fd);
done:
    f_mount(NULL, "1:", 0);
    if (image_fd >= 0) close(image_fd);
    image_fd = -1;
    image_readonly = false;
    pthread_mutex_unlock(&volume_lock);
    return result;
}

enum { SETUP_LOG_SECTORS = (128 << 20) / SECTOR };

static bool mount_setup_logs(FATFS *volume) {
    struct stat info;
    uint8_t header[SECTOR], table[GPT_TABLE_SECTORS * SECTOR];
    if (image_fd < 0 || fstat(image_fd, &info) || !S_ISREG(info.st_mode) ||
        info.st_size != (off_t)SETUP_LOG_SECTORS * SECTOR ||
        transfer(image_fd, header, sizeof header, SECTOR, false) ||
        transfer(image_fd, table, sizeof table, 2 * SECTOR, false) ||
        memcmp(header, "EFI PART", 8) || field_le(header + 12, 4) != 92 ||
        field_le(header + 72, 8) != 2 || field_le(header + 80, 4) != 128 ||
        field_le(header + 84, 4) != 128 ||
        field_le(header + 88, 4) != gpt_crc(table, sizeof table)) return false;
    uint32_t crc = (uint32_t)field_le(header + 16, 4);
    memset(header + 16, 0, 4);
    if (crc != gpt_crc(header, 92)) return false;
    uint64_t start = field_le(table + 32, 8);
    uint64_t last = field_le(table + 40, 8);
    if (start != ALIGNMENT || last != SETUP_LOG_SECTORS - GPT_TABLE_SECTORS - 2) return false;
    volumes[0] = (InstallerPartition){start, last - start + 1};
    FILINFO marker;
    return f_mount(volume, "0:", 1) == FR_OK && f_stat("0:/WINIOS.LOG", &marker) == FR_OK &&
           !(marker.fattrib & AM_DIR);
}

int winios_prepare_setup_logs(const char *path) {
    if (!path) return -1;
    pthread_mutex_lock(&volume_lock);
    FATFS volume;
    int result = -1;
    char *stage = NULL;
    image_fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (image_fd >= 0) {
        image_readonly = true;
        if (mount_setup_logs(&volume)) result = 0;
        goto done;
    }
    if (errno != ENOENT || asprintf(&stage, "%s.building.XXXXXX", path) < 0) goto done;
    image_fd = mkstemp(stage);
    if (image_fd < 0) goto done;
    volumes[0] = (InstallerPartition){ALIGNMENT, SETUP_LOG_SECTORS - ALIGNMENT - GPT_TABLE_SECTORS - 1};
    MKFS_PARM fat32 = {.fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = SECTOR};
    uint8_t buffer[4096];
    FIL marker;
    if (ftruncate(image_fd, (off_t)SETUP_LOG_SECTORS * SECTOR) ||
        write_media_gpt(SETUP_LOG_SECTORS - 1, true) ||
        f_mkfs("0:", &fat32, buffer, sizeof buffer) != FR_OK ||
        f_mount(&volume, "0:", 1) != FR_OK ||
        f_open(&marker, "0:/WINIOS.LOG", FA_CREATE_NEW | FA_WRITE) != FR_OK ||
        f_close(&marker) != FR_OK || fsync(image_fd)) goto done;
    result = link(stage, path);
done:
    f_mount(NULL, "0:", 0);
    if (image_fd >= 0) close(image_fd);
    if (stage) unlink(stage);
    free(stage);
    image_fd = -1;
    image_readonly = false;
    pthread_mutex_unlock(&volume_lock);
    return result;
}

static int export_log_file(const char *folder, const FILINFO *info) {
    char input[PATH_LENGTH], output[PATH_LENGTH];
    if (snprintf(input, sizeof input, "0:/Logs/%s", info->fname) >= (int)sizeof input ||
        snprintf(output, sizeof output, "%s/%s", folder, info->fname) >= (int)sizeof output) return -1;
    FIL file;
    if (f_open(&file, input, FA_READ) != FR_OK) return -1;
    int fd = open(output, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    int result = 0;
    uint8_t bytes[16384];
    uint64_t copied = 0;
    while (fd >= 0 && copied < info->fsize) {
        UINT count = info->fsize - copied > sizeof bytes ? sizeof bytes : (UINT)(info->fsize - copied);
        UINT read;
        if (f_read(&file, bytes, count, &read) != FR_OK || read != count ||
            transfer(fd, bytes, read, copied, true)) {
            result = -1;
            break;
        }
        copied += read;
    }
    f_close(&file);
    if (fd < 0) return -1;
    if (close(fd)) result = -1;
    if (result) unlink(output);
    return result;
}

int winios_export_setup_logs(const char *path, const char *folder) {
    if (!path || !folder) return -1;
    pthread_mutex_lock(&volume_lock);
    FATFS volume;
    DIR directory;
    FILINFO file;
    int count = -1;
    uint64_t total = 0;
    image_fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    image_readonly = true;
    if (!mount_setup_logs(&volume)) goto done;
    FRESULT status = f_opendir(&directory, "0:/Logs");
    if (status == FR_NO_PATH || status == FR_NO_FILE) { count = 0; goto done; }
    if (status != FR_OK) goto done;
    count = 0;
    while ((status = f_readdir(&directory, &file)) == FR_OK && file.fname[0]) {
        const char *suffix = strrchr(file.fname, '.');
        if ((file.fattrib & AM_DIR) || !safe_name(file.fname) || !suffix ||
            (strcasecmp(suffix, ".log") && strcasecmp(suffix, ".txt"))) continue;
        if (file.fsize > (128u << 20) - total || count >= 128 || export_log_file(folder, &file)) {
            count = -1;
            break;
        }
        total += file.fsize;
        ++count;
    }
    if (status != FR_OK) count = -1;
    f_closedir(&directory);
done:
    f_mount(NULL, "0:", 0);
    if (image_fd >= 0) close(image_fd);
    image_fd = -1;
    image_readonly = false;
    pthread_mutex_unlock(&volume_lock);
    return count;
}
