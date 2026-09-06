# VibeKey C5 最终板硬件方案（ESP32-C5 + vibetty + PDM 双麦立体声 + PCA9535CPW）

> 最终引脚与实现方案（无历史变更记录，仅保留最终结果）。  
> PDM 仅做**双麦录音（RX）**，无播放输出；GPIO15 为 PSRAM CS1，不可用（见 6.3 / 注意事项 3）。

## 1. 最终引脚分配总表

| 功能 | 信号 | 引脚 | 备注 |
|---|---|---|---|
| LCD SCLK | SPI_SCLK | **GPIO6** | SPI2_HOST，GPIO Matrix；已在你的模组实测可用 |
| LCD MOSI | SPI_DATA | **GPIO7** | 显示屏写数据，不使用 MISO |
| LCD RST | 复位 | **GPIO8** | |
| LCD DC | 命令/数据 | **GPIO9** | |
| LCD CS | 片选 | **GPIO10** | |
| LCD BLK | 背光 | **GPIO26** | LEDC PWM 调亮度，勿长期满占空 |
| RMT 彩灯 | WS2812 DIN | **GPIO27** | 1 个 RMT TX 通道驱动灯带 |
| I2C SDA | SDA | **GPIO2** | 接 PCA9535CPW，外部上拉 3.3V；固件内部上拉兜底（未焊时防悬空） |
| I2C SCL | SCL | **GPIO3** | 接 PCA9535CPW，外部上拉 3.3V；固件内部上拉兜底 |
| PCA9535 INT | 中断 | **GPIO28** | 下降沿唤醒 MCU，外部上拉；固件内部上拉兜底 |
| 编码器1 A/B | ENC1_A / ENC1_B | **GPIO0 / GPIO1** | PCNT **Unit0**，4 倍频；Strapping 脚，见注意事项 1 |
| 编码器2 A/B | ENC2_A / ENC2_B | **GPIO4 / GPIO5** | PCNT **Unit1** |
| 编码器3 A/B | ENC3_A / ENC3_B | **GPIO11 / GPIO12** | PCNT **Unit2**；UART0 默认脚，见注意事项 2 |
| PDM_CLK | BCK | **GPIO24** | PDM 时钟，双麦录音用 |
| PDM_DIN | 麦克风数据输入 | **GPIO23** | 双麦 DATA 并联，立体声输入 |
| USB D- / D+ | USB Serial/JTAG | **GPIO13 / GPIO14** | 仅烧录/日志/供电；ESP32-C5 不能做 USB HID |
| 保留/未用 | - | PCNT **Unit3**、GPIO16~22（含 19/20）、**GPIO25** 等 | GPIO28 已作 INT；GPIO15 为 PSRAM CS1 不可用 |

> ESP32-C5 的数字外设（除 USB 外）均可经 **GPIO Matrix** 映射到任意 GPIO；上表为最终物理连接。

---

## 2. LCD 连接

| LCD 信号 | ESP32-C5 引脚 | 说明 |
|---|---|---|
| SCLK / SPI_SCLK | GPIO6 | SPI2_HOST |
| MOSI / SPI_DATA | GPIO7 | 显示屏写数据 |
| RST | GPIO8 | 复位 |
| DC | GPIO9 | 命令/数据选择 |
| CS | GPIO10 | 片选 |
| BLK | GPIO26 | 背光，LEDC PWM 调光 |

- 屏幕为 **172x320（或 320x172，按实际方向配置）** 的 ST7789 类 SPI 屏；使用 **SPI2_HOST**（不要用 SPI0/1）。固件 v0.9.7 起逻辑坐标系为**横屏 320x172**（玻璃仍按 portrait 扫描，驱动用 PSRAM 全帧画布 + `st7789_commit()` 转置上屏，见 [vibekeyC5-firmware-plan.md](vibekeyC5-firmware-plan.md) 阶段5）。
- 屏 VDD 接 3.3V；若屏 IO 需 1.8V 要加电平转换。

---

## 3. RMT 彩灯

- **RMT TX 引脚：GPIO27**，驱动 WS2812B / SK6812 等可寻址 RGB 灯。
- 1 个 RMT TX 通道即可驱动一整条灯链（状态灯或按键背光串联均可）。
- 用于系统状态提示：
  - `waiting`（等待操作）：绿色常亮/慢呼吸
  - `working`（AI 生成中）：蓝色呼吸/流水
  - 错误/断开：红色闪烁
  - YOLO / 自动模式：紫色跑马灯

---

## 4. I2C 与 PCA9535CPW 扩展

### 4.1 I2C 总线
- **SDA = GPIO2**，**SCL = GPIO3**，均需**外部上拉到 3.3V**（常用 4.7k~10k）。
- PCA9535CPW（TSSOP24 的 PCA9535C）挂在该 I2C 上。
- A0/A1/A2 接地 → 7 位地址 **0x20**；写地址 **0x40**，读地址 **0x41**。

### 4.2 PCA9535CPW 引脚连接

| PCA9535CPW 引脚 | 连接 | 作用 |
|---|---|---|
| SCL(22) | ESP32-C5 **GPIO3** | I2C 时钟，外部上拉 3.3V |
| SDA(23) | ESP32-C5 **GPIO2** | I2C 数据，外部上拉 3.3V |
| A0/A1/A2(21/2/3) | 接地 | 地址 0x20 |
| INT(1) | ESP32-C5 **GPIO28** | 中断输出，上拉；按键唤醒 |
| VDD(24)/VSS(12) | 3.3V / GND | 0.1μF 去耦 |
| **IO0_0 ~ IO0_5** | 6 个 Clicky 按键 | 每脚外部 **10k 上拉到 3.3V**，按键另一端接 GND；按下=低；**不可悬空** |
| **IO0_6** | 编码器1 SW | 输入，同上拉 |
| **IO0_7** | 编码器2 SW | 输入，同上拉 |
| **IO1_0** | 编码器3 SW | 输入，同上拉 |
| IO1_1 ~ IO1_7 | 备用 | LED / 扩展按键（输出需外部上拉） |

### 4.3 初始化与读取
- 写方向：`0x40 → 0x06 → 0xFF → 0xFF`  
  （命令 0x06 = Configuration Port0；第 1 字节 0xFF 写 Port0 全输入；第 2 字节自动写 Port1 全输入）
- 读按键/SW：`0x40 → 0x00 → 重复 START → 0x41 → 读 1 字节`  
  得到 Input Port0：`bit0~5` = 6 个 Clicky 键，`bit6` = SW1，`bit7` = SW2；继续读下一字节得到 Input Port1，`bit0` = SW3。
- 如需反转极性（低有效读成 1）：用命令 `0x04/0x05` 写对应位为 1。
- **PCA9535CPW 是 C 版本（开漏、无内部上拉）**：所有输入脚必须外部上拉/下拉，禁止悬空；**INT（GPIO28）也需外部上拉**。

### 4.4 PCA9535CPW 未焊接的兼容性（固件必须支持）
- **当前 PCA9535CPW 尚未焊接**：固件必须**探测后优雅降级**——I2C 写 0x20 若无 ACK（器件不存在）即视为未焊接，跳过按键/SW 读取与 INT 配置，**设备仍可正常运行**（编码器 A/B 直连 ESP32 的 PCNT，不依赖 PCA9535）。
- 未焊接时以下 ESP32 侧 IO 悬空，固件必须**启用内部上拉**（`gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY)`）兜底：

| IO | 作用 | 未焊时的处理 |
|---|---|---|
| GPIO2（SDA）| I2C 数据 | 内部上拉，防悬空误触发/漏电 |
| GPIO3（SCL）| I2C 时钟 | 内部上拉 |
| GPIO28（INT）| 中断输入 | 内部上拉（下降沿检测需有确定高电平）|

- 焊接后：PCA9535CPW 为开漏 C 版本，输入脚仍需**外部 10k 上拉**；ESP32 内部上拉仅作兜底（阻值较大，抗干扰不如外部上拉）。

---

## 5. 编码器与 PCNT 映射

| 编码器 | PCNT Unit | A 相引脚 | B 相引脚 | 功能 | SW 引脚（PCA9535CPW） |
|---|---|---|---|---|---|
| 编码器1 | **Unit0** | **GPIO0** | **GPIO1** | 主滚动/Enter（ArrowUp/Down、PageUp/PageDown） | IO0_6 |
| 编码器2 | **Unit1** | **GPIO4** | **GPIO5** | Steer / 宏 / Retry / 切模型 | IO0_7 |
| 编码器3 | **Unit2** | **GPIO11** | **GPIO12** | 系统/实例切换/背光调节 | IO1_0 |
| 保留 | **Unit3** | 未用 | 未用 | 未来飞梭/Jog/脉冲计数 | - |

- 3 个编码器共占用 **6 个 C5 GPIO**（0/1、4/5、11/12）。
- 编码器 **A/B 必须直连 ESP32-C5**（走 PCNT，硬件 **4 倍频**正交计数）；**SW 只是普通按键，接 PCA9535CPW**。
- ESP32-C5 有 4 个 PCNT Unit，本方案使用 Unit0~2，**保留 Unit3**。
- A/B 相固件启用**内部上拉**（`GPIO_PULLUP_ONLY`），编码器公共端接 GND；现有代码已对 GPIO0/1/4/5 配置，新增编码器3（GPIO11/12）需同样配置。

---

## 6. PDM 双麦录音（RX）

### 6.1 最终 IO 占用
- **共 2 个 IO**：**GPIO24（PDM_CLK）、GPIO23（PDM_DIN）**。
- 仅 RX（2 麦录音），无 TX 播放。

### 6.2 麦克风连接（双麦立体声输入）

| 信号 | MIC1（左） | MIC2（右） | ESP32-C5 |
|---|---|---|---|
| VDD | 3.3V | 3.3V | 3.3V 供电 |
| GND | GND | GND | 共地 |
| CLK | 接 **GPIO24** | 接 **GPIO24** | 两麦 CLK 并联到 GPIO24 |
| DATA | 接 **GPIO23** | 接 **GPIO23** | 两麦 DATA 并联到 GPIO23 |
| L/R（SEL） | **接 GND**（左声道） | **接 3.3V/VDD**（右声道） | 硬连线，不接 MCU |

- 单根 PDM 数据线最多承载 **2 个麦克风**（左右声道）；本方案为双麦立体声输入。
- 若未来需 4 麦，必须弃用 PDM，改用 **ES7210 + TDM** 方案。

### 6.3 无播放输出（已放弃 PDM TX）
- 本方案**不做 PDM 播放**（喇叭/提示音不接），PDM 仅保留双麦录音 RX。
- GPIO15 原计划作 PDM_DOUT，但已确认 **GPIO15 = PSRAM CS1（SPICS1）**，启用 PSRAM 时不可作 GPIO（见注意事项 3）。
- ESP32-C5 的 PDM 全双工 TX 与 RX 时钟不兼容（实测会 CPU_LOCKUP），故彻底放弃 TX。

---

## 7. 固件实现流程

> **vibetty MQTT 协议速查（v0.4.0+，`{p}` = 实例前缀）**  
> 前缀 `{p}` = `{user}/{device}/{pid}/vibetty`：`user`=`[mqtt] username`（未配回退 `root`）、`device`=SHA256(machine-uid) 前 16 hex、`pid`=进程 pid（重启会变）。

| 方向 | Topic | Payload | QoS / retain | 用途 |
|---|---|---|---|---|
| ESP32→vibetty | `{p}/pty_in` | **raw 字节** | 0 / 否 | 单键 / 方向键 / Esc / Ctrl+C / 转义序列 |
| ESP32→vibetty | `{p}/control` | **JSON** `{"type","data"}` | 1 / 否 | `input_text`（文本）、`sync`（声明尺寸/开关推屏）、`scroll_up`/`scroll_down` |
| vibetty→ESP32 | `{p}/screen` | JPEG + 末尾 4 字节大端 scrollback offset | 0 / 否 | 仅 `-q high/medium/low` |
| vibetty→ESP32 | `{p}/screen_text` | **1 字节 tag + ANSI**（`0x00`=全屏基线、`0x01`=增量） | full 1 / delta 0 / 否 | 仅 `-q text`（**默认**） |
| vibetty→ESP32 | `{p}`（前缀本身） | presence JSON `{prefix,client_id,ts,title,state,format}` | 1 / **是（唯一 retained）** | 上线公告/心跳（15s）/状态 |

- **发现流程**：订阅 `+/+/+/vibetty`（或 `{user}/+/+/vibetty`）→ 收 retained presence → 解析 `prefix` + `format` → `format=="text"` 订 `{p}/screen_text`，否则订 `{p}/screen` → 发 `sync` 到 `{p}/control` 拿首帧（screen 都不是 retained，靠 sync 触发）。
- **presence 字段**：`state` ∈ `working`/`waiting`；`format` ∈ `text`/`high`/`medium`/`low`（决定订哪个 screen topic）；`ts` 用于心跳超时（>~30s 判离线）；LWT 空 payload = 实例下线。
- **sync**：`{"type":"sync","data":{"width":W,"height":H,"pixels":true,"close":false}}`；`pixels=true`(默认)=像素、`false`=字符列行；`close=true` 暂停自主推屏（省流量）、`false` 恢复。
- **输入映射**：单键 → `pty_in`；文本/命令 → `control` 的 `input_text`。`--auto-submit`（默认 true）**只对 `input_text` 生效**（补回车），`pty_in` 原样透传。

1. **外设初始化**
   - LEDC PWM：初始化 **GPIO26** 控制 LCD 背光。
   - SPI2：初始化 **GPIO6~10** 驱动 LCD（ST7789）。
   - I2C 主机：初始化 **SDA=GPIO2、SCL=GPIO3**（内部上拉兜底）；探测 PCA9535CPW(0x20)——**无 ACK 则跳过按键/SW/INT，继续运行**；有 ACK 则写配置命令 `0x06` 发 `0xFF 0xFF`（全输入），配置 **GPIO28** 为 INT 中断（下降沿，内部上拉兜底）。
   - PCNT：初始化 **Unit0（GPIO0/1）、Unit1（GPIO4/5）、Unit2（GPIO11/12）**，全部配置为 **4 倍频正交计数**；定时/中断读取增量。
   - RMT TX：初始化 **GPIO27**，注册 WS2812 灯带。
   - I2S PDM：配置 `bclk=GPIO24`、`din=GPIO23`、`ws=-1`（PDM 不用 WS）；**仅 RX，无 TX**。  
     - RX：`I2S_SLOT_MODE_STEREO` + raw PDM（双麦输入，2.048MHz 过采样）。  
     - 无 TX（播放已放弃）。  
     - 采样率 2.048MHz（raw PDM），GDMA 搬运，软件降采样到 16kHz PCM。
   - Wi-Fi STA + MQTT：连接运行 vibetty 的 PC 所在局域网，按 **vibetty 0.4.0+** 协议连接 broker（内置 `mqtt://<PC_IP>:1883` 或外部 broker）。
2. **vibetty 状态显示**
   - 订阅 `+/+/+/vibetty` 发现实例 → 解析 presence 拿 `prefix`/`format` → 按 `format` 订 `{p}/screen_text`（text）或 `{p}/screen`（JPEG）→ 发 `sync` 拿首帧。
   - 文本模式（默认 `-q text`）：解析 `screen_text` 首字节 tag（`0x00`=全屏基线，重置终端缓冲重放；`0x01`=增量，追加到缓冲），渲染 ANSI 到 LCD。
   - JPEG 模式（`-q high/medium/low`）：解码 `screen`（末尾 4 字节大端 = scrollback offset）缩放显示。
   - 解析 presence `state`（`working`/`waiting`），驱动 **GPIO27 RMT 彩灯**。
3. **输入映射到 vibetty keystrokes**
   - 读 PCNT 增量：编码器1→方向键/翻页；编码器2→宏/组合键；编码器3→切换 vibetty 多实例或调背光。
   - 读 PCA9535：6 Clicky 键默认 Accept=`Enter`、Reject=`Esc`、Retry=宏、YOLO=自动接受、ESC=`Escape`、Voice=按下启动录音；SW1=`Enter`、SW2=确认、SW3=返回。
   - **单键 / 方向键 / Esc / Ctrl+C / 转义序列** → 发布到 `{p}/pty_in`（raw 字节，QoS0）。
   - **文本 / 命令** → 发布到 `{p}/control`（JSON `{"type":"input_text","data":"..."}`，QoS1）；`--auto-submit`（默认 true）只对 `input_text` 生效，自动补回车执行。
4. **语音旁路（自定义扩展，非 vibetty 协议）**
   - vibetty 0.4.0 已移除服务端 ASR，且 ESP32-C5 无法本地跑 Whisper；识别走**自定义旁路**：Voice 键按下 → I2S PDM 从 `DIN=GPIO23` 采集双麦 PCM（16kHz/16bit）→ 经自定义 MQTT topic / TCP / WebSocket 送**自建** PC 端 ASR（Whisper.cpp server）。
   - ASR 文本 → 作为 `input_text` 经 `{p}/control` 发回 vibetty，注入终端，实现语音输入。
5. **低功耗 / Remap / OTA**
   - 空闲 Deep-sleep；PCA9535 INT（**GPIO28**）或编码器活动唤醒。
   - 键位/旋钮映射、Wi-Fi/MQTT 配置存 NVS；支持 MQTT 下发配置或 OTA。

---

## 8. 关键注意事项（最终版）

1. **GPIO0 / GPIO1 是 Strapping 引脚**：上电/复位时其电平会被锁存用于决定启动模式（尤其 GPIO0 为低可能进入下载模式）。用作编码器 A/B 相时：
   - 外部电路/编码器不得在上电时把 GPIO0/GPIO1 拉成错误启动电平；
   - 固件中 PCNT **不会自动配置内部上下拉**，必须显式调用 `gpio_set_pull_mode()` / `gpio_pullup_en()` / `gpio_pulldown_en()` 处理；
   - GPIO0/GPIO1 可通过 **GPIO Matrix** 路由到 PCNT，功能正常。
2. **GPIO11 / GPIO12 是 UART0 默认脚（U0TXD / U0RXD）**：用作编码器3 的 A/B 时，必须在 menuconfig 中**关闭 UART0 console**，将系统日志/console 输出改为 **USB Serial/JTAG（GPIO13/14）** 或 None；否则 UART0 会占用这两个脚并与 PCNT 计数冲突，启动日志还会干扰编码器。
3. **GPIO15 是 PSRAM CS1（已实测确认）**：ESP32-C5 的 `MSPI_IOMUX_PIN_NUM_CS1 = 15`，启用 PSRAM 时 GPIO15 固定作 SPI PSRAM 片选，**不可作 GPIO**。本方案已放弃 PDM 播放输出（原 DOUT=GPIO15 会破坏 PSRAM 导致崩溃）。
4. **PDM 音频能力（仅 RX）**：
   - `DIN=GPIO23` 接 2 个 MIC，L/R 硬连线，**RX 为立体声输入（2 通道）**。
   - ESP32-C5 的 PDM RX 为 **raw 模式**（无硬件 PDM2PCM），过采样率 ~2.048MHz，需软件降采样到 16kHz PCM。
   - 双麦最多 2 个；若需 4 麦需 ES7210 TDM 方案。
   - **无 PDM 播放输出**（GPIO15 为 PSRAM CS1，且全双工 TX 时钟冲突，已放弃）。
5. **PCA9535CPW 是开漏 C 版本（当前未焊接）**：6 个按键 + 3 个 SW 输入脚必须各接 **10k 上拉到 3.3V**，另一端接地；绝对不能悬空；**INT（GPIO28）也需外部上拉**；I2C 的 SDA/SCL（GPIO2/3）也需上拉。固件必须**先探测 0x20**，未焊接（无 ACK）时跳过按键/SW，仅靠编码器 A/B 运行；GPIO2/3/28 固件启用**内部上拉**兜底（见 4.4）。
6. **LCD 的 GPIO6~GPIO10 以你的模组实测为准**：当前已验证可用；若更换模组、启用封装内 PSRAM 或更改启动配置，需重新验证这些脚是否与内部 Flash/PSRAM 冲突。
7. **vibetty 协议与链路**：
   - 设备端 MQTT 固件必须为 **v0.4.0+**（新 topic、`screen_text` tag bytes、`Sync.pixels/close`、presence `format`），旧固件无法连接/显示；默认输出 `-q text`（`screen_text`），非 JPEG。
   - vibetty 入站只接收 **`pty_in`（键盘原始键）** 与 **`control`（`input_text`/`sync`/`scroll`）**，没有鼠标事件、没有音频 topic。语音识别走**自定义旁路**（PC 端自建 Whisper.cpp ASR），文本再经 `input_text` 回注 vibetty（vibetty 0.4.0 已移除服务端 ASR，且 ESP32-C5 无法本地跑 Whisper）。
   - PC 端必须启用 MQTT：配置 `[mqtt]` 且 `enable = true`（内置 broker 绑 `0.0.0.0:1883`，或外部 broker 均可）。
8. **ESP32-C5 无原生 USB HID（但有 BLE HID 能力）**：USB-C 的 GPIO13/14 是 USB Serial/JTAG，仅供电/烧录/日志；设备对电脑的 AI 控制在本方案里走 **MQTT keystrokes**（vibetty 链路）。C5 支持 BLE 键盘 HID，但仅作可选备用，不参与主流程（见 11.3）。

---

## 9. 总结

- **主控**：ESP32-C5（2.4/5GHz 双频 Wi-Fi 6 + BLE 5.0 + 802.15.4 Zigbee/Thread）。  
- **显示**：172x320 LCD，`GPIO6/7/8/9/10` + `BLK=GPIO26`（LEDC PWM）。  
- **灯效**：RMT 驱动 WS2812，`GPIO27`。  
- **I/O 扩展**：PCA9535CPW 挂 I2C（`SDA=GPIO2`、`SCL=GPIO3`，地址 `0x20`），扩展 6 个 Clicky 键 + 3 个编码器 SW；中断 `INT=GPIO28`。  
- **输入**：3 个编码器直连 PCNT —— `GPIO0/1`（Unit0）、`GPIO4/5`（Unit1）、`GPIO11/12`（Unit2），硬件 4 倍频；保留 Unit3。  
- **音频**：PDM 双麦立体声输入 `CLK=GPIO24`、`DIN=GPIO23`；PDM 双麦立体声录音 `CLK=GPIO24`、`DIN=GPIO23`（仅 RX，无播放输出）。  
- **控制链路**：通过 **vibetty over MQTT** 与 PC 上的 `claude`/`codex` 终端通信；设备回传 keystrokes 控制 AI Agent，PDM 麦克风音频旁路送 PC ASR，LCD + RMT 显示/提示状态，完整实现 AI 编码物理控制面。

---

## 10. 编译环境与工具链（ESP-IDF 6.0）

目标芯片 **ESP32-C5 N16R8**（Flash 16MB / PSRAM 8MB），ESP-IDF **v6.0**，工程名 `esp32c5_lcd147`。

| 工具 | 版本 / 路径 |
|---|---|
| ESP-IDF | `C:\esp\v6.0\esp-idf`（v6.0） |
| GCC（riscv32-esp-elf）| esp-15.2.0_20251204 |
| CMake | 4.0.3 |
| Ninja | 1.12.1 |
| Python | `C:\Espressif\tools\python\v6.0\venv`（3.10.11） |
| 串口 | COM38 |

**必须用 PowerShell 编译，不能用 Git Bash / MSys**：`idf.py` 检测到 `MSYSTEM` 环境变量会静默退出（exit 0 但实际不编译）。先清除 `MSYSTEM` 等继承变量、设置 `IDF_*` 环境变量与工具链 PATH，再执行（完整脚本见根目录 `build.ps1`，三选一 build/flash/monitor）：

```powershell
$env:IDF_PATH = 'C:\esp\v6.0\esp-idf\'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.0\venv\'
$env:ESP_IDF_VERSION = '6.0.0'   # 必须设，否则 idf.py 初始化抛 TypeError
$env:Path = "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;$env:Path"

Set-Location 'd:\XC\2026\hardwear\ESP32C5-LCD147'
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build
```

要点：
- `ESP_IDF_VERSION` 必须设置（组件管理器做版本比较时需要）。
- 工具链版本（GCC/CMake/Ninja）必须与 `build` 缓存一致，否则触发大量组件重编；**不要删除 `build` 目录**，源码未变时直接增量编译。
- 首次或切换芯片时先 `idf.py set-target esp32c5`。
- 烧录/监视：`idf.py -p COM38 flash monitor`；退出监视器 `Ctrl+]`。

---

## 11. Wi-Fi 与蓝牙在本项目中的作用 + 网络配置

### 11.1 当前网络配置

| 项 | 值 |
|---|---|
| 2.4G SSID | `360WiFi-91868` |
| 5G SSID | `360WiFi-91868_5G` |
| 密码 | `18602191868` |
| **连接目标** | **2.4G 或 5G 均可**（C5 双频；推荐 2.4G，覆盖/穿墙更稳） |

- **ESP32-C5 支持 2.4 & 5 GHz 双频 Wi-Fi 6（802.11ax）**，`360WiFi-91868`（2.4G）与 `360WiFi-91868_5G`（5G）都能连；默认建议连 2.4G。
- vibetty PC 需与 ESP32 在**同一局域网/网段**；若 2.4G 与 5G 同网段可互通，否则 PC 连与 ESP32 同一频段的网络。
- broker 地址：内置 broker 绑 `0.0.0.0:1883`，ESP32 连 `mqtt://<PC_IP>:1883`（`<PC_IP>` 为 PC 在局域网内的 IP）。

### 11.2 Wi-Fi 的作用（主数据链路）

- **唯一数据通道**：经 Wi-Fi 连路由器 → 与运行 vibetty 的 PC 同局域网 → 通过 MQTT broker 双向通信：
  - **下行（状态查看）**：收 presence（AI `working`/`waiting` 状态）与 `screen`/`screen_text`（终端画面）。
  - **上行（交付）**：发 keystrokes（`pty_in` 单键 / `control` 文本）控制 `claude`/`codex`。
- 断线自动重连是可靠性的关键（对应固件计划阶段 8）。

### 11.3 蓝牙的作用（BLE 5.0，辅助通道）

ESP32-C5 的蓝牙是 **BLE 5.0（仅低功耗蓝牙，无经典蓝牙）**，不参与 vibetty 数据链路，只作辅助：

- **配网（主要用途）**：用 BLE（或 SoftAP）给设备配置 Wi-Fi SSID/密码 + broker 地址，避免把密码硬编码在固件里；ESP-IDF 有现成 `wifi_prov` 框架。
- **可选 BLE HID（备用输入）**：ESP32-C5 支持 BLE 键盘/鼠标 HID，可作备用/增强输入通道；但本方案主控链路是 MQTT（vibetty），BLE HID 不参与主流程。
- **注意区分**：C5 无 **USB HID**（GPIO13/14 仅 USB Serial/JTAG），但有 **BLE HID** 能力——两者不是一回事。