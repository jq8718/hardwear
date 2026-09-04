# ESP32-C5 LCD147

这是 ESP32-C5 N16R8 的 ESP-IDF 工程，目前实现三部分功能：

1. **WS2812 彩灯**：`GPIO27` 通过 RMT 发送 GRB 数据，单颗 LED 以低亮度循环彩虹色。
2. **ST7789 液晶**：4 线 SPI（40 MHz）驱动 172x320 的 LCD147，背光由 `GPIO26` 控制。
3. **两个旋转编码器**：编码器 1（`GPIO0/1`）、编码器 2（`GPIO4/5`）用 PCNT 正交解码（4 倍频），16 位原始计数与 32 位累加计数实时显示在 LCD 上。

目录中的 `TFT-147-HSD-ST7789-4WSPI-STM32` 是 LCD 移植参考的 STM32F103 工程。

**编译方案复用**：跨 ESP-IDF 项目可复用的完整编译/烧录/监视脚本见下文「从 Git Bash / MSys 终端编译」一节——尤其注意其中清除 `MSYSTEM` 环境变量的步骤，否则 `idf.py` 在 Git Bash/MSys 下会静默退出（看似成功、实际未编译）。

## 当前功能

| 模块 | 配置 |
|---|---|
| 目标芯片 | ESP32-C5 N16R8 |
| PSRAM | 8 MB，已启用并加入 `malloc()` 分配 |

### WS2812 彩灯

| 项目 | 配置 |
|---|---|
| WS2812 DIN | GPIO27 |
| RMT 分辨率 | 10 MHz |
| LED 颜色 | 低亮度彩虹循环，GRB 数据动态变化 |
| LED 数量 | 1 |

### ST7789 液晶

| 项目 | 配置 |
|---|---|
| 接口 | 4 线 SPI，40 MHz |
| 分辨率 | 172 x 320（横向偏移 34） |
| 背光 | GPIO26，高电平点亮 |
| 内容 | 编码器计数实时显示 |

### 旋转编码器

| 项目 | 配置 |
|---|---|
| 编码器 1 | A=GPIO0，B=GPIO1 |
| 编码器 2 | A=GPIO4，B=GPIO5 |
| 解码方式 | PCNT 正交解码，4 倍频 |
| 内部上拉 | 已启用（GPIO_PULLUP_ONLY） |

接线时将 WS2812 的 `DIN` 接到 GPIO27，并共地。WS2812 的供电电压按实际模块规格连接；首次测试建议使用较低亮度，避免单颗 LED 电流过大。

## 编译环境

在 PowerShell 中初始化环境：

```powershell
$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://C:\Espressif\tools\'
$env:OPENOCD_SCRIPTS = 'C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts\'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools\'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011\'
$env:IDF_PATH = 'C:\esp\v6.0\esp-idf\'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0\venv\'
$env:ESP_IDF_VERSION = '6.0.0'

$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
```

本板为 N16R8：Flash 为 16 MB，PSRAM 为 8 MB。项目已在 `sdkconfig` 中启用 `CONFIG_SPIRAM`，使用 ESP-IDF 默认的 `CONFIG_SPIRAM_USE_MALLOC` 将 PSRAM 纳入普通 `malloc()` 分配；小于 16 KB 的分配优先使用内部 RAM，并保留 32 KB 内部 RAM 供 DMA、任务栈等必须使用内部 RAM 的场景。

## 从 Git Bash / MSys 终端编译（可复用于其他项目）

直接在 Git Bash / MSys（或任何会注入 `MSYSTEM` 环境变量的终端）里运行 `idf.py` 会**静默失败**：`idf.py` 入口处检测到 `MSYSTEM` 后只打印一句 `MSys/Mingw is no longer supported...` 就退出，**不执行 `main()`**，且退出码仍是 0，看起来像"成功"但实际没有编译/烧录。

正确做法是用 PowerShell 运行，并在脚本开头清除从 MSys 继承的环境变量。下面是结合上文环境变量、可直接复用的脚本模板；换项目时只需改第 2 步的 `IDF_*` 路径、第 3 步的工具链 PATH 和第 4 步的 `Set-Location`。

```powershell
$ErrorActionPreference = 'Stop'

# 1) 清除 MSys 继承的环境变量（不删会导致 idf.py 静默退出）
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_CARCH -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:OSTYPE -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:TERM -ErrorAction SilentlyContinue

# 2) ESP-IDF 环境变量（按本机安装路径调整）
$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://C:\Espressif\tools\'
$env:OPENOCD_SCRIPTS = 'C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts\'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools\'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011\'
$env:IDF_PATH = 'C:\esp\v6.0\esp-idf\'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0\venv\'
$env:ESP_IDF_VERSION = '6.0.0'

# 3) 工具链加入 PATH（版本必须与 build 缓存一致，否则触发全编译）
$env:Path = "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;$env:Path"

$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'

# 4) 进入工程目录并执行动作（三选一）
Set-Location 'd:\XC\2026\hardwear\ESP32C5-LCD147'
& $idfPython $idfPy build               # 增量编译
# & $idfPython $idfPy -p COM38 flash     # 烧录
# & $idfPython $idfPy -p COM38 monitor   # 串口监视
exit $LASTEXITCODE
```

将上述内容保存为 `build.ps1`，在 Git Bash 里这样调用：

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

要点：

- **`MSYSTEM` 是根因**：`idf.py` 入口处的 `if 'MSYSTEM' in os.environ` 分支只打印警告、不调用 `main()`，所以命令"成功"（exit 0）却什么都没做；清除它才会走到真正的编译/烧录逻辑。
- **`ESP_IDF_VERSION` 必须设置**：任何 `idf.py` 动作在初始化阶段，组件管理器都会读取 `ESP_IDF_VERSION` 做版本比较；缺省时为 `None` 会抛 `TypeError: expected string or bytes-like object`。VSCode 靠 `customExtraVars` 里的 `ESP_IDF_VERSION` 规避了此问题。
- **工具链 PATH 必须与 `build` 缓存一致**：GCC / CMake / Ninja 版本对不上会触发大量 ESP-IDF 组件重编。其绝对路径已固化在 `build` 缓存的 `CMakeCache.txt`、`build.ninja`、`compile_commands.json` 里，换项目时以目标工程的缓存为准。

## VSCode 工具链固定

项目 `.vscode/settings.json` 已固定为与当前构建缓存相同的工具环境：

| 工具 | 版本或路径 |
|---|---|
| ESP-IDF | `C:\esp\v6.0\esp-idf`，v6.0-dirty |
| GCC | `esp-15.2.0_20251204`，15.2.0 |
| CMake | `4.0.3` |
| Ninja | `1.12.1` |
| Python | `C:\Espressif\tools\python\v6.0\venv`，3.10.11 |
| 串口 | `COM38` |

VSCode 的 ESP-IDF 扩展会从 `IDF_TOOLS_PATH` 自动选择对应的 GCC、CMake 和 Ninja。请不要在 VSCode 中切换 ESP-IDF 版本，也不要删除 `build` 目录；源码未变化时应直接使用现有 CMake/Ninja 构建缓存执行增量编译。若修改了 `sdkconfig` 或切换了目标芯片，第一次重新配置出现较长时间是正常的，之后会恢复增量编译。

可在 VSCode 的 ESP-IDF 诊断命令中确认当前环境，或检查 `build\CMakeCache.txt` 中的 `CMAKE_COMMAND` 和 `CMAKE_MAKE_PROGRAM` 是否分别指向上述 CMake / Ninja。GCC 路径由 `CMAKE_TOOLCHAIN_FILE`（`toolchain-esp32c5.cmake`）决定，实际编译器路径可见于 `build\compile_commands.json`。

注意：不要在已经加载了其他 ESP-IDF 版本的 PowerShell 中直接执行 `idf.py`。如果 `IDF_PATH` 指向旧版本，会出现新版本 `idf.py` 与旧版本 `idf_py_actions` 混用的 `ImportError`。每次编译前应先执行上面的环境变量设置，确认 `$env:IDF_PATH` 和 `$env:IDF_PYTHON_ENV_PATH` 指向本项目使用的 ESP-IDF 6.0 环境。

检查环境：

```powershell
& $idfPython $idfPy --version
# 预期输出：ESP-IDF v6.0-dirty（或 ESP-IDF v6.0.x）
```

如果 `C:\Espressif\tools\python\v6.0\venv\Scripts\python.exe` 不存在，在已设置上述环境变量的 PowerShell 中执行一次：

```powershell
& "$env:IDF_PATH\install.ps1" esp32c5
```

该命令会补齐 ESP32-C5 所需工具和 Python 依赖。安装完成后重新打开 PowerShell，并重新执行环境变量设置。

## 编译与烧录

首次使用或切换目标芯片时执行：

```powershell
& $idfPython $idfPy set-target esp32c5
```

编译：

```powershell
& $idfPython $idfPy build
```

日常修改源码时直接执行上面的 `build`，它会复用 `build` 目录中的 CMake/Ninja 缓存，只重新编译发生变化的目标。不要删除 `build` 目录，也不要在同一个工程目录同时启动多个 Build/Flash 任务。

连接开发板后，将 `COMxx` 替换为实际串口：

```powershell
& $idfPython $idfPy -p COMxx flash monitor
```

如果刚刚已经成功编译，只想烧录现有镜像，请使用下面的命令。`idf.py flash` 默认会先执行 `build`；当 Ninja 版本或构建日志格式变化时，可能重新编译大量 ESP-IDF 组件。直接调用同一 Python 环境中的 esptool 可以避免这次重复编译：

```powershell
$env:Path = "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;$env:Path"
& $idfPython -m esptool --chip esp32c5 -p COM38 -b 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x2000 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x10000 build\esp32c5_lcd147.bin
```

上面的直接烧录命令只适用于 `build` 中的镜像来自本次工程、目标芯片为 ESP32-C5 且 Flash 配置为 16 MB 的情况。修改 `sdkconfig`、切换目标芯片或更换 ESP-IDF 版本后，必须先执行一次 `set-target` 和完整 `build`。

退出串口监视器使用 `Ctrl+]`。烧录完成并重启后，GPIO27 上的 WS2812 开始低亮度彩虹循环，LCD 点亮并显示两个编码器的 RAW/ACC 计数。

## 编译验证

当前构建配置已核对为 ESP32-C5、ESP-IDF 6.0.0，开发板实际 Flash 为 16 MB，当前 factory 应用分区为 1 MB。`build` 目录中已生成以下文件，说明工程已完成编译：

```text
build/esp32c5_lcd147.bin
build/esp32c5_lcd147.elf
```

如果切换了芯片、ESP-IDF 版本或工程路径，建议先执行 `idf.py set-target esp32c5`，再执行 `idf.py build`。

## 运行日志检查

在 `COM38`、115200 baud 下烧录并运行后，确认出现以下日志：

```text
I (...) app_init: Project name:     esp32c5_lcd147
I (...) app_init: ESP-IDF:          v6.0-dirty
I (...) esp_psram: Found 8MB PSRAM device
I (...) ws2812: Memory: internal free=..., PSRAM total=..., PSRAM free=...
I (...) ws2812: WS2812 rainbow started on GPIO27
I (...) st7789: ST7789 initialized, backlight on
I (...) encoder: encoders ready: enc1 GPIO0/1, enc2 GPIO4/5
I (...) ws2812: LCD encoder display ready
```

首次运行旧配置时，日志曾提示检测到 16 MB Flash 但镜像头声明为 2 MB。现已将 `CONFIG_ESPTOOLPY_FLASHSIZE` 改为 `16MB` 并重新编译烧录。监视器还曾因工具链目录未加入 `PATH` 而无法调用 `riscv32-esp-elf-addr2line`；启动日志本身不受影响，调试时应确保该工具的 `bin` 目录已加入 `PATH`。

本次实际验证结果：使用上述固定工具链编译、直接烧录后，COM38 日志显示 `SPI Flash Size : 16MB`、`Found 8MB PSRAM device`、`SPI SRAM memory test OK`，并将 `8192K` PSRAM 加入堆分配器；应用随后输出 `WS2812 rainbow started on GPIO27`。若再次看到 `ninja: warning: build log version is too new`，不要立即删除 `build`，先确认 CMake/Ninja 版本与本 README 一致，并优先使用已有镜像的直接烧录命令。

## LCD147 参考接线

| LCD147 | ESP32-C5 N16R8 |
|---|---|
| SCL | GPIO6 |
| SDA / MOSI | GPIO7 |
| RST / RES | GPIO8 |
| AS / DC | GPIO9 |
| CS | GPIO10 |
| BL / BLK | GPIO26 |
| WS2812 DIN | GPIO27 |
| VCC | 3.3 V |
| GND | GND |

LCD147 的有效区域为 `172x320`，ST7789 横向偏移为 `34`。LCD 驱动已移植，通过 4 线 SPI（40 MHz）点亮显示，背光由 GPIO26 控制。

## 编码器参考接线

两个旋转编码器通过 PCNT（脉冲计数器）正交解码驱动，A/B 相均启用内部上拉，旋转方向决定计数值增减，计数值实时显示在 LCD 上。

| 编码器 | A 相 | B 相 |
|---|---|---|
| 编码器 1 | GPIO0 | GPIO1 |
| 编码器 2 | GPIO4 | GPIO5 |

- A/B 相内部上拉已启用（`GPIO_PULLUP_ONLY`），编码器公共端接 GND。
- 每个编码器占用一个 PCNT 单元、两个通道做正交解码（`driver/pulse_cnt.h`），对 A/B 两相的 4 个跳变沿全部计数，即 4 倍频。PCNT 硬件计数寄存器为 16 位，回绕阈值 `low_limit/high_limit` 只能配置在 `-32768 ~ 32767`；开启 `accum_count` 后由中断做 32 位软件累加，`pcnt_unit_get_count()` 返回的累计值可达 32 位范围（约 ±21 亿），不会在 ±32767 处回绕。
- LCD 上半区显示 `ENC1`（绿色标签）的 `RAW`（琥珀色，16 位原始计数）与 `ACC`（白色，32 位累加计数）；下半区显示 `ENC2`（青色标签）的 `RAW` 与 `ACC`，中间有分隔线。
