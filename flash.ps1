$ErrorActionPreference = 'Stop'

Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_CARCH -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:OSTYPE -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:TERM -ErrorAction SilentlyContinue

$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://C:\Espressif\tools\'
$env:OPENOCD_SCRIPTS = 'C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts\'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools\'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011\'
$env:IDF_PATH = 'C:\esp\v6.0\esp-idf\'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0\venv\'
$env:ESP_IDF_VERSION = '6.0.0'

$env:Path = "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;$env:Path"

$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'

Set-Location 'd:\XC\2026\hardwear\ESP32C5-LCD147'
& $idfPython -m esptool --chip esp32c5 -p COM38 -b 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x2000 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x10000 build\esp32c5_lcd147.bin
exit $LASTEXITCODE
