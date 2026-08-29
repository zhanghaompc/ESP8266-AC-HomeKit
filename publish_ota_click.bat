@echo off
cd /d "%~dp0"
echo.
echo ESP8266 OTA Publisher
echo ---------------------
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish_ota.ps1"
echo.
if errorlevel 1 (
  echo Publish failed. Please check the messages above.
) else (
  echo Publish finished.
)
echo.
pause
