/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_INSTALLER_WINPE_H
#define WINIOS_INSTALLER_WINPE_H

#include "installer_disk.h"

/* Copy the ISO's boot.wim inside work_directory and wire its boot image to
 * launch standard Windows Setup against the separate installer payload. */
int winios_prepare_winpe_wim(const char *iso_path, const char *work_directory,
                            WinIOSInstallerProgress progress, void *context,
                            char *error, size_t error_size);
/* Patch WinPE to deploy a verified install.wim or install.esd index with inbox Windows tools. */
int winios_prepare_direct_winpe_wim(const char *iso_path, const char *work_directory,
                                   const char *target_guid, int image_index,
                                   WinIOSInstallerProgress progress, void *context,
                                   char *error, size_t error_size);

#endif
