@echo off
:: Opens an IDF-enabled shell in the project root.
:: From here you can cd into any subproject and run idf.py commands.
::
:: esp-bridge (MCU-side C5):
::   cd esp-bridge
::   idf.py set-target esp32c5
::   idf.py build
::   idf.py -p COMx flash monitor
::
:: ESP-Hosted slave (Pi-side C5):
::   See esp-hosted-pi\build_and_flash.sh (run in WSL or on Linux)

call C:\esp\v6.0\esp-idf\export.bat
cd /d C:\KlipperESPwifi
cmd /k
