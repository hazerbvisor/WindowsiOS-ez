/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tpm_device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libtpms/tpm_error.h>
#include <libtpms/tpm_library.h>
#include <libtpms/tpm_nvfilename.h>
#include <libtpms/tpm_tis.h>

enum {
    NO_LOCALITY = -1, LOCALITIES = 5, TPM_BUFFER = 4096, TPM_HEADER = 10,
    ACCESS = 0, INTF_CAP = 0x14, STATUS = 0x18, FIFO = 0x24, INTERFACE_ID = 0x30,
    EXT_FIFO = 0x80, EXT_FIFO_END = 0xbc, DID_VID = 0xf00, REVISION = 0xf04,
    ACCESS_VALID = 0x80, ACTIVE = 0x20, SEIZED = 0x10, SEIZE = 0x08, PENDING = 0x04, REQUEST = 0x02,
    STS_VALID = 0x80, READY = 0x40, GO = 0x20, DATA = 0x10, EXPECT = 0x08, RETRY = 0x02
};

#define STS_TPM2 (1u << 26)

struct WinIOSTpm {
    int directory;
    int locality;
    uint8_t requested;
    uint8_t seized;
    bool ready;
    bool started;
    bool failed;
    bool initializing_existing;
    uint8_t command[TPM_BUFFER];
    uint32_t written;
    uint32_t expected;
    uint8_t *response;
    uint32_t response_size;
    uint32_t response_capacity;
    uint32_t read_offset;
};

static WinIOSTpm *active;

static bool valid_state_name(const char *name) {
    return strcmp(name, TPM_PERMANENT_ALL_NAME) == 0 || strcmp(name, TPM_SAVESTATE_NAME) == 0 ||
           strcmp(name, TPM_VOLATILESTATE_NAME) == 0;
}

static int transfer_state(int fd, void *buffer, size_t size, bool writing) {
    uint8_t *bytes = buffer;
    while (size) {
        ssize_t result = writing ? write(fd, bytes, size) : read(fd, bytes, size);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        bytes += result;
        size -= (size_t)result;
    }
    return 0;
}

static TPM_RESULT load_nv(uint8_t **data, uint32_t *length, uint32_t number, const char *name) {
    if (number || !active || !valid_state_name(name)) return TPM_FAIL;
    int fd = openat(active->directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? TPM_RETRY : TPM_FAIL;
    struct stat st;
    TPM_RESULT result = TPM_FAIL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 4 << 20) {
        uint8_t *state = malloc((size_t)st.st_size);
        if (state && !transfer_state(fd, state, (size_t)st.st_size, false)) {
            *data = state;
            *length = (uint32_t)st.st_size;
            result = TPM_SUCCESS;
        } else {
            free(state);
        }
    }
    close(fd);
    return result;
}

static TPM_RESULT store_nv(const uint8_t *data, uint32_t length, uint32_t number, const char *name) {
    if (number || !active || !valid_state_name(name)) return TPM_FAIL;
    if (active->initializing_existing && strcmp(name, TPM_PERMANENT_ALL_NAME) == 0) return TPM_FAIL;
    char temp[64];
    snprintf(temp, sizeof(temp), ".nv-%08x%08x", (unsigned)arc4random(), (unsigned)arc4random());
    int fd = openat(active->directory, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return TPM_FAIL;
    /* The write branch of transfer_state leaves the referenced bytes unchanged. */
    int error = transfer_state(fd, (void *)data, length, true) || fsync(fd);
    if (close(fd)) error = -1;
    if (!error) error = renameat(active->directory, temp, active->directory, name);
    if (error) {
        unlinkat(active->directory, temp, 0);
        return TPM_FAIL;
    }
    return TPM_SUCCESS;
}

static TPM_RESULT delete_nv(uint32_t number, const char *name, TPM_BOOL must_exist) {
    if (number || !active || !valid_state_name(name)) return TPM_FAIL;
    if (!unlinkat(active->directory, name, 0) || (!must_exist && errno == ENOENT)) return TPM_SUCCESS;
    return TPM_FAIL;
}

static TPM_RESULT get_locality(TPM_MODIFIER_INDICATOR *locality, uint32_t number) {
    if (number || !active) return TPM_FAIL;
    *locality = active->locality < 0 ? 0 : (TPM_MODIFIER_INDICATOR)active->locality;
    return TPM_SUCCESS;
}

static TPM_RESULT get_presence(TPM_BOOL *present, uint32_t number) {
    if (number) return TPM_FAIL;
    *present = 0;
    return TPM_SUCCESS;
}

static struct libtpms_callbacks callbacks = {
    .sizeOfStruct = sizeof(callbacks),
    .tpm_nvram_loaddata = load_nv,
    .tpm_nvram_storedata = store_nv,
    .tpm_nvram_deletename = delete_nv,
    .tpm_io_getlocality = get_locality,
    .tpm_io_getphysicalpresence = get_presence,
};

static void clear_command(WinIOSTpm *tpm) {
    tpm->written = 0;
    tpm->expected = 0;
    tpm->response_size = 0;
    tpm->read_offset = 0;
    tpm->ready = false;
    memset(tpm->command, 0, sizeof(tpm->command));
}

WinIOSTpm *winios_tpm_open(const char *path) {
    if (!path || active || (mkdir(path, 0700) && errno != EEXIST)) return NULL;
    WinIOSTpm *tpm = calloc(1, sizeof(*tpm));
    if (!tpm) return NULL;
    tpm->directory = open(path, O_DIRECTORY | O_RDONLY | O_NOFOLLOW);
    tpm->locality = NO_LOCALITY;
    if (tpm->directory < 0) {
        free(tpm);
        return NULL;
    }
    active = tpm;
    if (TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2) || TPMLIB_RegisterCallbacks(&callbacks) ||
        winios_tpm_reset(tpm)) {
        winios_tpm_close(tpm);
        return NULL;
    }
    return tpm;
}

int winios_tpm_reset(WinIOSTpm *tpm) {
    if (!tpm || tpm != active) return -1;
    if (tpm->started) TPMLIB_Terminate();
    tpm->started = false;
    tpm->locality = NO_LOCALITY;
    tpm->requested = 0;
    tpm->seized = 0;
    clear_command(tpm);
    TPMLIB_SetBufferSize(TPM_BUFFER, NULL, NULL);
    struct stat existing;
    bool has_permanent_state = fstatat(tpm->directory, TPM_PERMANENT_ALL_NAME, &existing,
                                      AT_SYMLINK_NOFOLLOW) == 0;
    if (!has_permanent_state && errno != ENOENT) {
        tpm->failed = true;
        return -1;
    }
    tpm->initializing_existing = has_permanent_state;
    TPM_RESULT result = TPMLIB_MainInit();
    tpm->initializing_existing = false;
    tpm->started = result == TPM_SUCCESS;
    tpm->failed = !tpm->started || (has_permanent_state && TPMLIB_WasManufactured());
    return tpm->failed ? -1 : 0;
}

void winios_tpm_close(WinIOSTpm *tpm) {
    if (!tpm || tpm != active) return;
    if (tpm->started) TPMLIB_Terminate();
    active = NULL;
    close(tpm->directory);
    free(tpm->response);
    free(tpm);
}

static uint32_t command_size(const uint8_t *bytes) {
    return (uint32_t)bytes[2] << 24 | (uint32_t)bytes[3] << 16 | (uint32_t)bytes[4] << 8 | bytes[5];
}

static bool is_fifo(uint32_t reg) {
    return (reg >= FIFO && reg < FIFO + 4) || (reg >= EXT_FIFO && reg <= EXT_FIFO_END);
}

static bool valid_access(uint64_t offset, unsigned size) {
    return size && size <= 8 && offset < WINIOS_TPM_MMIO_SIZE &&
           size <= 4096 - (offset & 4095);
}

static uint32_t status(WinIOSTpm *tpm) {
    uint32_t remaining = 0;
    uint32_t value = STS_VALID | STS_TPM2;
    if (tpm->read_offset < tpm->response_size) {
        remaining = tpm->response_size - tpm->read_offset;
        value |= DATA;
    } else if (tpm->ready) {
        if (!tpm->written) value |= READY;
        uint32_t wanted = tpm->expected ? tpm->expected : TPM_BUFFER;
        if (wanted > tpm->written) {
            remaining = wanted - tpm->written;
            value |= EXPECT;
        }
    }
    if (remaining > 64) remaining = 64;
    return value | remaining << 8;
}

uint64_t winios_tpm_read(void *opaque, uint64_t offset, unsigned size) {
    WinIOSTpm *tpm = opaque;
    if (!tpm || tpm->failed || !valid_access(offset, size)) return UINT64_MAX;
    int locality = (int)(offset >> 12);
    uint32_t reg = (uint32_t)(offset & 4095);
    uint8_t flag = 1u << locality;
    if (is_fifo(reg)) {
        uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i) {
            uint8_t byte = 0xff;
            if (tpm->locality == locality && tpm->read_offset < tpm->response_size)
                byte = tpm->response[tpm->read_offset++];
            result |= (uint64_t)byte << (i * 8);
        }
        return result;
    }
    uint32_t value = 0;
    switch (reg & ~3u) {
    case ACCESS:
        value = ACCESS_VALID;
        TPM_BOOL established = 0;
        if (TPM_IO_TpmEstablished_Get(&established) == TPM_SUCCESS && !established) value |= 1;
        if (tpm->locality == locality) value |= ACTIVE;
        if (tpm->requested & ~flag) value |= PENDING;
        if (tpm->seized & flag) value |= SEIZED;
        break;
    case INTF_CAP: value = 3u << 28; break; /* TIS 2.0, polling only. */
    case STATUS: value = tpm->locality == locality ? status(tpm) : STS_TPM2; break;
    case INTERFACE_ID: value = (1u << 13) | (1u << 8); break; /* FIFO, five localities. */
    case DID_VID: value = 0x00011014; break;
    case REVISION: value = 1; break;
    default: break;
    }
    uint64_t result = value >> (8 * (reg & 3));
    return size < 8 ? result & ((UINT64_C(1) << (8 * size)) - 1) : result;
}

static void write_access(WinIOSTpm *tpm, int locality, uint8_t value) {
    uint8_t flag = 1u << locality;
    if (value & SEIZED) tpm->seized &= ~flag;
    if ((value & SEIZE) && tpm->locality != locality && locality > tpm->locality) {
        if (tpm->locality != NO_LOCALITY) tpm->seized |= 1u << tpm->locality;
        tpm->locality = locality;
        tpm->requested &= ~flag;
        clear_command(tpm);
    } else if ((value & ACTIVE) && tpm->locality == locality) {
        tpm->locality = NO_LOCALITY;
        tpm->requested &= ~flag;
        for (int next = LOCALITIES - 1; next >= 0; --next) {
            if (tpm->requested & (1u << next)) {
                tpm->locality = next;
                tpm->requested &= ~(1u << next);
                break;
            }
        }
        clear_command(tpm);
    } else if ((value & REQUEST) && tpm->locality != locality) {
        if (tpm->locality == NO_LOCALITY) tpm->locality = locality;
        else tpm->requested |= flag;
    }
}

static void submit(WinIOSTpm *tpm) {
    uint32_t size = 0;
    if (tpm->written < TPM_HEADER || tpm->written != tpm->expected) return;
    TPM_RESULT result = TPMLIB_Process(&tpm->response, &size, &tpm->response_capacity,
                                      tpm->command, tpm->written);
    tpm->ready = false;
    tpm->read_offset = 0;
    if (result != TPM_SUCCESS || size < TPM_HEADER || size > TPM_BUFFER) {
        tpm->failed = true;
        return;
    }
    tpm->response_size = size;
}

void winios_tpm_write(void *opaque, uint64_t offset, uint64_t value, unsigned size) {
    WinIOSTpm *tpm = opaque;
    if (!tpm || tpm->failed || !valid_access(offset, size)) return;
    int locality = (int)(offset >> 12);
    uint32_t reg = (uint32_t)(offset & 4095);
    if (reg == ACCESS) {
        write_access(tpm, locality, (uint8_t)value);
        return;
    }
    if (locality != tpm->locality) return;
    if ((reg & ~3u) == STATUS) {
        if (reg != STATUS) {
            value <<= (reg & 3u) * 8;
            if (locality >= 3 && (value & (1u << 25))) TPM_IO_TpmEstablished_Reset();
            return;
        }
        if (locality >= 3 && (value & (1u << 25))) TPM_IO_TpmEstablished_Reset();
        if (value & READY) {
            clear_command(tpm);
            tpm->ready = true;
        } else if (value & GO) {
            if (tpm->ready) submit(tpm);
        } else if (value & RETRY) {
            tpm->read_offset = 0;
        }
        return;
    }
    if (!is_fifo(reg) || !tpm->ready) return;
    for (unsigned i = 0; i < size; ++i) {
        if (tpm->written >= TPM_BUFFER || (tpm->expected && tpm->written >= tpm->expected)) break;
        tpm->command[tpm->written++] = (uint8_t)(value >> (i * 8));
        if (tpm->written == 6) {
            tpm->expected = command_size(tpm->command);
            if (tpm->expected < TPM_HEADER || tpm->expected > TPM_BUFFER) {
                clear_command(tpm);
                break;
            }
        }
    }
}
