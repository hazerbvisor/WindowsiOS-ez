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

void winios_jit_install_trap_handler(void);
bool winios_jit_is_debugger_attached(void);
bool winios_jit_acquire(size_t bytes, uint64_t requested_ram_mib, bool detach_after);
bool winios_jit_is_live(void);
WinIosDualMapping winios_jit_mapping(void);
void *winios_jit_guest_ram(uint64_t *out_bytes);
const char *winios_jit_status_text(void);
size_t winios_available_memory_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
