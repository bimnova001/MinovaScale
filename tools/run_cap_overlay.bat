@echo off
cd /d D:\MinovaUpscaler
build\Release\minova_capture.exe --window "MinovaUpscaler Phase 1 PoC" --out 1280 720 --overlay --seconds 6 > cap_overlay.log 2>&1
echo EXITCODE=%ERRORLEVEL% >> cap_overlay.log
