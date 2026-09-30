@echo off
REM Build/flash helper for the ESP32-S3 firmware.
REM
REM idf.py refuses to run under MSys/MinGW, and the ESP-IDF Windows installer's
REM export scripts mis-resolve this machine's Python venv (they look for
REM idf5.5_py3.12_env; the installed venv is idf5.5_py3.11_env). This wrapper
REM sets the environment natively and calls idf.py through the venv interpreter.
REM
REM Usage:  scripts\esp_idf_build.cmd build
REM         scripts\esp_idf_build.cmd -p COM4 flash
REM         scripts\esp_idf_build.cmd -p COM4 monitor
setlocal
REM idf.py aborts when MSYSTEM is set, which it is inside Git-Bash even when
REM this batch runs. Clear it so the native build proceeds.
set "MSYSTEM="
set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.5"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
set "PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PATH%\tools;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%"
cd /d "%~dp0..\firmware\esp32s3_fomo"
"%PYTHON%" "%IDF_PATH%\tools\idf.py" %*
endlocal
