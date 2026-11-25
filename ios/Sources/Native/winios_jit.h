/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Dual-mapped RX/RW JIT allocator for iOS 17..27 (including iOS 26/27 TXM).
 * Adapted from Henge (Leviidev/Henge, GPL-2.0-or-later).
 */
#ifndef WINIOS_JIT_H
#define WINIOS_JIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WinIosDualMapping {
    uint8_t *rw_addr;
    uint8_t *rx_addr;
    size_t   size;
} WinIosDualMapping;

#define WINIOS_JIT_DEFAULT_BYTES ((size_t)384u << 20)
/* Allow requests through the largest experimental preset. The OS and debugger
 * still decide whether a low executable mapping can actually be obtained. */
#define WINIOS_JIT_EXPERIMENTAL_MAX_BYTES ((size_t)1536u << 20)

#define WINIOS_JIT_LOW_LIMIT UINT64_C(0x7000000000)
#define WINIOS_GUEST_RAM_HEADROOM_MIB UINT64_C(768)

static inline bool winios_jit_rx_fits(uint64_t start, size_t bytes) {
    return start != 0 && bytes != 0 && bytes <= WINIOS_JIT_LOW_LIMIT && start <= WINIOS_JIT_LOW_LIMIT - bytes;
}

static inline bool winios_guest_ram_fits(uint64_t requested, uint64_t budget) {
    return requested >= 4096 && budget > WINIOS_GUEST_RAM_HEADROOM_MIB &&
           requested <= budget - WINIOS_GUEST_RAM_HEADROOM_MIB;
}

void winios_jit_install_trap_handler(void);
bool winios_jit_is_debugger_attached(void);
bool winios_jit_acquire(size_t bytes, uint64_t requested_ram_mib, bool detach_after);
bool winios_jit_is_live(void);
WinIosDualMapping winios_jit_mapping(void);
void *winios_jit_guest_ram(uint64_t *out_bytes);
const char *winios_jit_status_text(void);
const char *winios_jit_ram_status_text(void);
size_t winios_available_memory_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
