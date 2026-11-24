/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_INSTALLER_DISK_H
#define WINIOS_INSTALLER_DISK_H

#include <stddef.h>
#include <stdint.h>

/* Build a GPT disk with a FAT32 EFI/WinPE partition and an exFAT payload partition.
 * The ISO stays read-only. Output is published only after a complete, synced copy.
 * When supplied, boot_wim_path replaces the ISO boot image in the output.
 * Returning nonzero from progress aborts preparation. */
typedef int (*WinIOSInstallerProgress)(uint64_t copied, uint64_t total, void *context);
int winios_build_installer_disk(const char *iso_path, const char *output_path, const char *boot_wim_path,
                                const char *driver_directory, const char *answer_path,
                                WinIOSInstallerProgress progress, void *context,
                                char *error, size_t error_size);

/* Enable the answer file only for a verified blank primary. Pass a NULL
 * primary to disable automation before a guest reboot. */
int winios_configure_installer_answer(const char *installer, const char *primary);

/* Inspect the primary disk's validated EFI partition without changing it. */
int winios_has_windows_boot_files(const char *disk_path, uint64_t start, uint64_t sectors);

#endif
