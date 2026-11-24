/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_TPM_DEVICE_H
#define WINIOS_TPM_DEVICE_H

#include <stdint.h>

/* TCG FIFO interface, five 4 KiB locality windows. */
#define WINIOS_TPM_MMIO_SIZE 0x5000u

typedef struct WinIOSTpm WinIOSTpm;

/* libtpms is process-global. The current VM engine owns one instance at a time. */
WinIOSTpm *winios_tpm_open(const char *state_directory);
void winios_tpm_close(WinIOSTpm *tpm);
int winios_tpm_reset(WinIOSTpm *tpm);
uint64_t winios_tpm_read(void *opaque, uint64_t offset, unsigned size);
void winios_tpm_write(void *opaque, uint64_t offset, uint64_t value, unsigned size);

#endif
