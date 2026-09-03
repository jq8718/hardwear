$ErrorActionPreference = 'Stop'

# 1) Clear MSys-inherited env vars (otherwise idf.py silently exits)
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_CARCH -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:OSTYPE -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:TERM -ErrorAction SilentlyContinue

# 2) ESP-IDF env vars
$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://C:\Espressif\tools\'
$env:OPENOCD_SCRIPTS = 'C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts\'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools\'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011\'
$env:IDF_PATH = 'C:\esp\v6.0\esp-idf\'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0\venv\'
$env:ESP_IDF_VERSION = '6.0.0'

# 3) Toolchain PATH
$env:Path = "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;$env:Path"

$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'

# 4) Enter project dir and build
Set-Location 'd:\XC\2026\hardwear\ESP32C5-LCD147'
& $idfPython $idfPy build
exit $LASTEXITCODE
