@echo off
setlocal DisableDelayedExpansion
title MetalGPU Driver Installer - v0.6 R2
echo WindowsiOS-ez MetalGPU Driver Installer
echo.
echo Back up the VM first. Run as administrator with VirtIO-GPU enabled.
choice /C YN /N /M "Continue with the experimental signed driver? [Y/N] "
if errorlevel 2 goto cancel
if not errorlevel 1 goto cancel
set "WORK=%TEMP%\MetalGPU-%RANDOM%-%RANDOM%"
mkdir "%WORK%" || goto fail
set "ZIP=%WORK%\driver.zip"
echo [1/4] Obtaining official UTM ARM64X driver...
if exist "%~dp0viogpu3d-arm64x-signed.zip" (
  copy /y "%~dp0viogpu3d-arm64x-signed.zip" "%ZIP%" || goto fail
) else (
  curl.exe --fail --location --show-error --connect-timeout 15 --max-time 180 --output "%ZIP%" "https://github.com/utmapp/viogpu-wddm/releases/download/v0.6/viogpu3d-arm64x-signed.zip" || goto fail
)
echo [2/4] Checking SHA-256...
certutil.exe -hashfile "%ZIP%" SHA256 > "%WORK%\hash.txt"
if errorlevel 1 goto fail
findstr.exe /I /X /C:"723131a1d0a8b7f01037b7efe3b6ac7b90f034734460885fba0edd7d8c75425f" "%WORK%\hash.txt" >nul
if errorlevel 1 (
  echo ERROR: Driver checksum did not match.
  goto fail
)
echo [3/4] Extracting signed driver...
mkdir "%WORK%\driver" || goto fail
tar.exe -xf "%ZIP%" -C "%WORK%\driver" || goto fail
set "INF="
for /r "%WORK%\driver" %%F in (viogpu3d.inf) do if exist "%%F" set "INF=%%F"
if not defined INF goto fail
echo [4/4] Installing with Windows PnPUtil...
echo "%INF%"
pnputil.exe /add-driver "%INF%" /install
set "RC=%ERRORLEVEL%"
if "%RC%"=="3010" goto success
if not "%RC%"=="0" goto fail
:success
echo.
echo Driver staging completed. Restart Windows and check Device Manager.
echo Installation does not prove hardware acceleration.
pause
rmdir /s /q "%WORK%"
exit /b 0
:fail
echo.
echo Installation failed. Review the output above.
echo If PnPUtil reports access denied, run as administrator.
echo Temporary files, if any, are at "%WORK%".
pause
exit /b 1
:cancel
echo Cancelled.
pause
exit /b 0
