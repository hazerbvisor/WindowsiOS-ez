/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "installer_winpe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <udfread/udfread.h>
#include <wimlib.h>

enum { BUFFER_SIZE = 512 << 10 };

static const char launch_ini[] =
    "[LaunchApps]\r\n"
    "%SYSTEMROOT%\\System32\\cmd.exe, \"/c %SYSTEMROOT%\\System32\\winios-setup.cmd\"\r\n";

static const char launch_cmd[] =
    "@echo off\r\n"
    "wpeinit\r\n"
    "set \"WINIOS_IMAGE=\"\r\n"
    "set \"WINIOS_DRIVERS=\"\r\n"
    "for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (\r\n"
    "  if exist %%D:\\WINIOS.MEDIA (\r\n"
    "    if exist %%D:\\sources\\install.wim set \"WINIOS_IMAGE=%%D:\\sources\\install.wim\"\r\n"
    "    if exist %%D:\\sources\\install.esd set \"WINIOS_IMAGE=%%D:\\sources\\install.esd\"\r\n"
    "    if exist %%D:\\drivers\\NetKVM\\netkvm.inf set \"WINIOS_DRIVERS=%%D:\\drivers\\NetKVM\"\r\n"
    "  )\r\n"
    ")\r\n"
    "if not defined WINIOS_IMAGE (\r\n"
    "  echo Windows Setup could not find the WINIOS.MEDIA payload volume.\r\n"
    "  pause\r\n"
    "  exit /b 1\r\n"
    ")\r\n"
    "if not exist \"%SYSTEMDRIVE%\\sources\\setup.exe\" (\r\n"
    "  echo Windows Setup is missing from this Windows PE image.\r\n"
    "  pause\r\n"
    "  exit /b 1\r\n"
    ")\r\n"
    "if defined WINIOS_DRIVERS (\r\n"
    "  \"%SYSTEMDRIVE%\\sources\\setup.exe\" /InstallFrom \"%WINIOS_IMAGE%\" /InstallDrivers \"%WINIOS_DRIVERS%\"\r\n"
    ") else (\r\n"
    "  \"%SYSTEMDRIVE%\\sources\\setup.exe\" /InstallFrom \"%WINIOS_IMAGE%\"\r\n"
    ")\r\n"
    "if errorlevel 1 (\r\n"
    "  echo Windows Setup exited before completing. Review the Setup message.\r\n"
    "  pause\r\n"
    ")\r\n";

static char *child_path(const char *directory, const char *name) {
    char *path = NULL;
    if (asprintf(&path, "%s/%s", directory, name) < 0) return NULL;
    return path;
}

static int write_all(int fd, const void *data, size_t size) {
    const char *bytes = data;
    while (size) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        bytes += written;
        size -= (size_t)written;
    }
    return 0;
}

static int write_script(const char *path, const char *contents, size_t length) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) return -1;
    int result = write_all(file, contents, length);
    if (close(file)) result = -1;
    if (result) unlink(path);
    return result;
}

static UDFDIR *open_sources(UDFDIR *root) {
    struct udfread_dirent entry;
    while (udfread_readdir(root, &entry)) {
        if (entry.d_type == UDF_DT_DIR && entry.d_name && !strcasecmp(entry.d_name, "sources"))
            return udfread_opendir_at(root, entry.d_name);
    }
    return NULL;
}

static UDFFILE *open_boot_wim(UDFDIR *sources) {
    struct udfread_dirent entry;
    while (udfread_readdir(sources, &entry)) {
        if (entry.d_type == UDF_DT_REG && entry.d_name && !strcasecmp(entry.d_name, "boot.wim"))
            return udfread_file_openat(sources, entry.d_name);
    }
    return NULL;
}

static const char *extract_wim(const char *iso_path, const char *wim_path,
                               WinIOSInstallerProgress progress, void *context) {
    const char *error = "Windows PE could not be found in the ISO.";
    udfread *volume = udfread_init();
    UDFDIR *root = NULL, *sources = NULL;
    UDFFILE *wim = NULL;
    char *buffer = NULL;
    int output = -1;
    if (!volume || udfread_open(volume, iso_path) != 0 || !(root = udfread_opendir(volume, "/")) ||
        !(sources = open_sources(root)) || !(wim = open_boot_wim(sources))) goto done;
    int64_t size = udfread_file_size(wim);
    if (size <= 0 || size > UINT32_MAX) goto done;
    output = open(wim_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    buffer = malloc(BUFFER_SIZE);
    if (output < 0 || !buffer) {
        error = "Windows PE could not be staged. Check available storage.";
        goto done;
    }

    uint64_t total = (uint64_t)size;
    uint64_t copied = 0;
    while (copied < total) {
        size_t count = total - copied > BUFFER_SIZE ? BUFFER_SIZE : (size_t)(total - copied);
        if (udfread_file_read(wim, buffer, count) != (ssize_t)count || write_all(output, buffer, count)) {
            error = "Windows PE could not be copied from the ISO.";
            goto done;
        }
        copied += count;
        if (progress && progress(copied, total, context)) {
            error = "Windows PE preparation was cancelled.";
            goto done;
        }
    }
    error = NULL;
done:
    if (output >= 0) {
        if (close(output) && !error) error = "Windows PE could not be saved.";
        if (error) unlink(wim_path);
    }
    free(buffer);
    if (wim) udfread_file_close(wim);
    if (sources) udfread_closedir(sources);
    if (root) udfread_closedir(root);
    if (volume) udfread_close(volume);
    return error;
}

static const char *update_winpe(const char *wim_path, char *ini, char *cmd) {
    WIMStruct *wim = NULL;
    struct wimlib_wim_info info;
    char ini_target[] = "/Windows/System32/winpeshl.ini";
    char cmd_target[] = "/Windows/System32/winios-setup.cmd";
    struct wimlib_update_command updates[] = {
        {.op = WIMLIB_UPDATE_OP_ADD, .add = {.fs_source_path = ini, .wim_target_path = ini_target,
                                            .add_flags = WIMLIB_ADD_FLAG_NO_ACLS}},
        {.op = WIMLIB_UPDATE_OP_ADD, .add = {.fs_source_path = cmd, .wim_target_path = cmd_target,
                                            .add_flags = WIMLIB_ADD_FLAG_NO_ACLS}}
    };

    int result = wimlib_open_wim(wim_path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim);
    if (!result) result = wimlib_get_wim_info(wim, &info);
    if (!result && (!info.boot_index || info.boot_index > info.image_count)) {
        wimlib_free(wim);
        return "The Windows PE WIM has no default boot image.";
    }
    if (!result) result = wimlib_update_image(wim, (int)info.boot_index, updates, 2, 0);
    if (!result) result = wimlib_overwrite(wim, 0, 1);
    if (wim) wimlib_free(wim);
    return result ? wimlib_get_error_string((enum wimlib_error_code)result) : NULL;
}

int winios_prepare_winpe_wim(const char *iso_path, const char *work_directory,
                            WinIOSInstallerProgress progress, void *context,
                            char *error, size_t error_size) {
    const char *failure = "The Windows PE working directory is unavailable.";
    if (!iso_path || !work_directory) {
        if (error && error_size) snprintf(error, error_size, "%s", failure);
        return -1;
    }
    char *wim = child_path(work_directory, "boot.wim");
    char *ini = child_path(work_directory, "winpeshl.ini");
    char *cmd = child_path(work_directory, "winios-setup.cmd");
    bool owned_wim = false, owned_ini = false, owned_cmd = false;
    if (!wim || !ini || !cmd) goto done;

    failure = extract_wim(iso_path, wim, progress, context);
    if (failure) goto done;
    owned_wim = true;
    failure = "Windows PE startup files could not be staged.";
    if (write_script(ini, launch_ini, sizeof(launch_ini) - 1)) goto done;
    owned_ini = true;
    if (write_script(cmd, launch_cmd, sizeof(launch_cmd) - 1)) goto done;
    owned_cmd = true;
    failure = update_winpe(wim, ini, cmd);

done:
    if (owned_cmd) unlink(cmd);
    if (owned_ini) unlink(ini);
    if (owned_wim && failure) unlink(wim);
    free(wim);
    free(ini);
    free(cmd);
    if (!failure) return 0;
    if (error && error_size) snprintf(error, error_size, "%s", failure);
    return -1;
}
