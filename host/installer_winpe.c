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

#include "disc_image.h"
#include <wimlib.h>

enum { BUFFER_SIZE = 512 << 10 };

static const char launch_ini[] =
    "[LaunchApps]\r\n"
    "%SYSTEMROOT%\\System32\\cmd.exe, \"/c %SYSTEMROOT%\\System32\\winios-setup.cmd\"\r\n";

static const char launch_cmd[] =
    "@echo off\r\n"
    "setlocal EnableExtensions DisableDelayedExpansion\r\n"
    "if /i \"%~1\"==\"/collect-disk-logs\" goto captureDiskLogs\r\n"
    "wpeinit\r\n"
    "set \"PE_RC=%ERRORLEVEL%\"\r\n"
    "set \"MEDIA=\"\r\n"
    "set \"LOGVOL=\"\r\n"
    "set \"TRIES=0\"\r\n"
    "set \"ATTEMPT=%RANDOM%-%RANDOM%\"\r\n"
    "for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (\r\n"
    "  if exist \"%%D:\\WINIOS.V2\" if exist \"%%D:\\WINIOS.MEDIA\" (\r\n"
    "    set \"MEDIA=%%D:\"\r\n"
    "    set /a TRIES+=1 >nul\r\n"
    "  )\r\n"
    "  if not defined LOGVOL if exist \"%%D:\\WINIOS.LOG\" set \"LOGVOL=%%D:\"\r\n"
    ")\r\n"
    "set \"REPORT=%SystemDrive%\\WinIOS-Logs\"\r\n"
    "if defined LOGVOL set \"REPORT=%LOGVOL%\\Logs\"\r\n"
    "if not exist \"%REPORT%\\\" mkdir \"%REPORT%\"\r\n"
    "if not exist \"%REPORT%\\\" set \"REPORT=%SystemDrive%\\WinIOS-Logs\"\r\n"
    "if not exist \"%REPORT%\\\" mkdir \"%REPORT%\"\r\n"
    "if not defined LOGVOL echo The writable Setup diagnostics disk is unavailable. Logs may be lost at restart.\r\n"
    ">>\"%REPORT%\\launcher.log\" echo [%DATE% %TIME%] wpeinit exit code: %PE_RC%; media: %MEDIA%; diagnostics: %REPORT%\r\n"
    "if not \"%TRIES%\"==\"1\" goto badMedia\r\n"
    "set \"SETUP_EXE=%MEDIA%\\sources\\setup.exe\"\r\n"
    "if not exist \"%SETUP_EXE%\" goto badMedia\r\n"
    "set \"IMAGE=\"\r\n"
    "if exist \"%MEDIA%\\sources\\install.wim\" set \"IMAGE=install.wim\"\r\n"
    "if exist \"%MEDIA%\\sources\\install.esd\" (\r\n"
    "  if defined IMAGE goto badMedia\r\n"
    "  set \"IMAGE=install.esd\"\r\n"
    ")\r\n"
    "if not defined IMAGE goto badMedia\r\n"
    "set \"DRIVERS=\"\r\n"
    "set \"ANSWER=\"\r\n"
    "if exist \"%MEDIA%\\drivers\\NetKVM\\netkvm.inf\" if exist \"%MEDIA%\\drivers\\NetKVM\\netkvm.cat\" if exist \"%MEDIA%\\drivers\\NetKVM\\netkvm.sys\" set DRIVERS=/InstallDrivers \"%MEDIA%\\drivers\\NetKVM\"\r\n"
    "if exist \"%MEDIA%\\WINIOS.AUTO\" if exist \"%MEDIA%\\WINIOS-SETUP.XML\" set ANSWER=/Unattend:\"%MEDIA%\\WINIOS-SETUP.XML\"\r\n"
    "if /i \"%~1\"==\"/manual\" goto manual\r\n"
    "\r\n"
    ":launch\r\n"
    "pushd \"%MEDIA%\\sources\" || goto badMedia\r\n"
    ">>\"%REPORT%\\launcher.log\" echo [%DATE% %TIME%] Setup: \"%SETUP_EXE%\" source: \"%MEDIA%\\sources\\%IMAGE%\" options: %DRIVERS% %ANSWER%\r\n"
    "start \"\" /wait \"%SETUP_EXE%\" /NoReboot /CopyLogs \"%REPORT%\" %DRIVERS% %ANSWER%\r\n"
    "set \"SETUP_RC=%ERRORLEVEL%\"\r\n"
    "popd\r\n"
    ">>\"%REPORT%\\launcher.log\" echo [%DATE% %TIME%] setup.exe exit code: %SETUP_RC%\r\n"
    "echo Windows Setup exit code: %SETUP_RC%\r\n"
    "call :captureLogs\r\n"
    "if not \"%SETUP_RC%\"==\"0\" goto recovery\r\n"
    "echo Review the Setup result. Restart only after Setup has finished staging Windows.\r\n"
    "set \"REBOOT=\"\r\n"
    "set /p \"REBOOT=Type R to restart, or press Enter for the recovery prompt: \"\r\n"
    "if /i \"%REBOOT%\"==\"R\" wpeutil reboot\r\n"
    "goto recovery\r\n"
    "\r\n"
    ":badMedia\r\n"
    ">>\"%REPORT%\\launcher.log\" echo [%DATE% %TIME%] Setup source validation failed. Marked payload volumes found: %TRIES%.\r\n"
    "echo Setup media is incomplete. Rebuild the Windows Setup disk in the VM library.\r\n"
    "call :captureLogs\r\n"
    "cmd.exe /k\r\n"
    "exit /b 2\r\n"
    "\r\n"
    ":recovery\r\n"
    "echo Logs: %REPORT%\r\n"
    "echo Type M to restart Windows Setup without the selected edition or driver flags.\r\n"
    "echo Press Enter for a recovery command prompt.\r\n"
    "set \"RECOVERY=\"\r\n"
    "set /p \"RECOVERY=Recovery option: \"\r\n"
    "if /i \"%RECOVERY%\"==\"M\" goto manual\r\n"
    "cmd.exe /k\r\n"
    "exit /b %SETUP_RC%\r\n"
    "\r\n"
    ":manual\r\n"
    "set \"ATTEMPT=%RANDOM%-%RANDOM%\"\r\n"
    "set \"DRIVERS=\"\r\n"
    "set \"ANSWER=\"\r\n"
    ">>\"%REPORT%\\launcher.log\" echo [%DATE% %TIME%] Manual Setup requested\r\n"
    "goto launch\r\n"
    "\r\n"
    ":captureLogs\r\n"
    "call :copyPanther \"%SystemRoot%\\Panther\" \"panther-winpe\"\r\n"
    "call :copyPanther \"%SystemDrive%\\$WINDOWS.~BT\\Sources\\Panther\" \"staging-winpe\"\r\n"
    "start \"Windows disk log collection\" /min cmd.exe /d /c call \"%~f0\" /collect-disk-logs\r\n"
    "exit /b 0\r\n"
    "\r\n"
    ":captureDiskLogs\r\n"
    "for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (\r\n"
    "  call :copyPanther \"%%D:\\Windows\\Panther\" \"panther-%%D\"\r\n"
    "  call :copyPanther \"%%D:\\$WINDOWS.~BT\\Sources\\Panther\" \"staging-%%D\"\r\n"
    "  call :copyPanther \"%%D:\\$WINDOWS.~BT\\Sources\\Rollback\" \"rollback-%%D\"\r\n"
    "  if exist \"%%D:\\Windows\\INF\\setupapi.dev.log\" copy /y \"%%D:\\Windows\\INF\\setupapi.dev.log\" \"%REPORT%\\setupapi-%ATTEMPT%-%%D.log\" >nul\r\n"
    ")\r\n"
    "exit /b 0\r\n"
    "\r\n"
    ":copyPanther\r\n"
    "for %%F in (setupact.log setuperr.log) do (\r\n"
    "  if exist \"%~1\\%%F\" copy /y \"%~1\\%%F\" \"%REPORT%\\%ATTEMPT%-%~2-%%F\" >nul\r\n"
    ")\r\n"
    "exit /b 0\r\n";

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

static const char *extract_wim(const char *iso_path, const char *wim_path,
                               WinIOSInstallerProgress progress, void *context,
                               char *diagnostic, size_t diagnostic_size) {
    const char *error = "Windows PE could not be found in the ISO.";
    DiscImage *image = disc_open_windows(iso_path, diagnostic, diagnostic_size);
    if (!image) return diagnostic;
    DiscFile *wim = disc_open_path(image, "sources/boot.wim");
    char *buffer = NULL;
    int output = -1;
    if (!wim) goto done;
    int64_t size = disc_file_size(wim);
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
        if (disc_file_read(wim, buffer, count) != (ssize_t)count || write_all(output, buffer, count)) {
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
    disc_file_close(wim);
    disc_close(image);
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

    char diagnostic[512];
    failure = extract_wim(iso_path, wim, progress, context, diagnostic, sizeof(diagnostic));
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
