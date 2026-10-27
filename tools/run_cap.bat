@echo off
cd /d D:\MinovaUpscaler
build\Release\minova_capture.exe --window "MinovaUpscaler Phase 1 PoC" --out 1280 720 --force-res 800 450 --seconds 20 > cap_out.log 2>&1
echo EXITCODE=%ERRORLEVEL% > cap_exit.log
