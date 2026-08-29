@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo.
echo ESP8266 OTA 快速发布
echo --------------------
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish_ota.ps1"
echo.
if errorlevel 1 (
  echo 发布失败，请查看上面的错误信息。
) else (
  echo 发布完成。
)
echo.
pause
