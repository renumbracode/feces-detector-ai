#!/usr/bin/env bash
# Activate the ESP-IDF 5.5.5 environment for this repo.
#
# Why this exists: the Windows ESP-IDF installer (C:\Espressif) does not provide
# an export.bat usable from a Git-Bash/agent shell, and Initialize-Idf.ps1
# mis-derives the venv name (it looks for idf5.5_py3.12_env, but the installed
# venv is idf5.5_py3.11_env). The installed tool directories are also nested
# under version folders, so this script globs for them rather than hardcoding.
#
# Usage:  source scripts/esp_idf_env.sh
# Then:   idf.py -p COM4 build
export IDF_TOOLS_PATH="C:\\Espressif"
export IDF_PATH="C:\\Espressif\\frameworks\\esp-idf-v5.5.5"
export IDF_PYTHON_ENV_PATH="C:\\Espressif\\python_env\\idf5.5_py3.11_env"
export ESP_ROM_ELF_DIR="C:\\Espressif\\tools\\esp-rom-elfs\\20241011"

T="/c/Espressif/tools"

# Add every bin/ dir under the toolchain roots that actually exists.
for root in \
  "$T/xtensa-esp-elf" \
  "$T/xtensa-esp-elf-gdb" \
  "$T/esp32ulp-elf" \
  "$T/riscv32-esp-elf" \
  "$T/riscv32-esp-elf-gdb" \
  "$T/cmake" \
  "$T/ninja" \
  "$T/idf-exe" \
  "$T/idf-python" \
  "$T/python"
do
  [ -d "$root" ] || continue
  while IFS= read -r d; do
    [ -d "$d" ] && export PATH="$d:$PATH"
  done < <(find "$root" -maxdepth 3 -type d -name bin 2>/dev/null)
  # Some tools put the executable at the version dir root, not under bin/.
  while IFS= read -r f; do
    export PATH="$(dirname "$f"):$PATH"
  done < <(find "$root" -maxdepth 2 -type f -name "*.exe" 2>/dev/null)
done

# The IDF python env provides idf.py and esptool.
IDF_PY="/c/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe"
export IDF_PYTHON="$IDF_PY"
[ -d "/c/Espressif/python_env/idf5.5_py3.11_env/Scripts" ] && \
  export PATH="/c/Espressif/python_env/idf5.5_py3.11_env/Scripts:$PATH"
export PATH="$IDF_PATH/tools:$PATH"
