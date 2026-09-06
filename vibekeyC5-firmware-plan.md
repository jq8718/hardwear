# VibeKey C5 固件编程计划（ESP32-C5 + vibetty）

> 对应硬件方案 [vibekeyC5.md](vibekeyC5.md)。目标：把现有「WS2812 + ST7789 + 双编码器」工程，扩展为完整 VibeKey——3 编码器 + 按键（PCA9535 可选）+ PDM 音频 + Wi-Fi/MQTT 接入 vibetty，实现 AI 状态查看与按键/旋钮/语音交付。

## 0. 现状盘点（已实现）

| 模块 | 现状 | 代码 |
|---|---|---|
| WS2812 | GPIO27 RMT 驱动，单颗彩虹循环 | [main.c](main/main.c)、[ws2812_encoder.c](main/ws2812_encoder.c) |
| LCD | ST7789 4 线 SPI（40MHz，172x320），文本渲染 + 背光 GPIO26 | [st7789.c](main/st7789.c) |
| 编码器 | 2 路 PCNT 4 倍频（GPIO0/1、4/5），内部上拉已配 | [encoder.c](main/encoder.c) |
| 编译 | ESP-IDF 6.0，`project(esp32c5_lcd147)` | [CMakeLists.txt](CMakeLists.txt) |

尚未实现：编码器3、I2C/PCA9535、按键、INT 中断、PDM 音频、Wi-Fi、MQTT、vibetty 协议、输入映射、状态灯。

## 1. 目标模块划分

```
main/
  CMakeLists.txt          # 扩展 SRCS + PRIV_REQUIRES
  main.c                  # 重构为 app 主循环 + 状态机
  encoder.c/.h            # 扩展为 3 路 + 增量读取接口
  st7789.c/.h             # 已存在（LCD + 文本渲染），按需加 ANSI 简易渲染
  + lcd_vt.c/.h           # AI 状态视图：状态头 + VT100 子集微型终端（28x38）
  ws2812_encoder.c/.h     # 已存在（RMT 编码器）
  + i2c_io.c/.h           # I2C 主机 + PCA9535 探测/读取/优雅降级
  + audio.c/.h            # I2S PDM RX(双麦录音)
  + wifi_mqtt.c/.h        # Wi-Fi STA + MQTT client + vibetty 0.4.0+ 协议
  + input_map.c/.h        # 编码器/按键 → keystrokes / 命令 映射
  + state_led.c/.h        # RMT 状态灯（waiting/working/error/yolo）
  + nvs_config.c/.h       # NVS 配置持久化（Wi-Fi/broker/键位）
```

CMakeLists `PRIV_REQUIRES` 需新增：`esp_driver_i2c`、`esp_driver_i2s`、`esp_wifi`、`esp_netif`、`nvs_flash`、`mqtt`、`json`（以及现有 `esp_driver_rmt/spi/gpio/pcnt esp_hal_pcnt`）。

## 2. 分阶段实施

### 阶段 1：编码器3 + UART0 console 迁移
- **目标**：把 GPIO11/12 腾给编码器3，系统日志改走 USB Serial/JTAG（GPIO13/14）。
- **menuconfig**：关闭 UART0 console（`CONFIG_ESP_CONSOLE_UART_DEFAULT=n`），console 改 USB Serial/JTAG；否则 UART0 占用 GPIO11/12 且启动日志干扰 PCNT。
- **encoder.c**：把 `encoder_add()` 泛化为 3 路数组（Unit0/1/2），新增 `encoder3_get_count()/get_raw()`、增量读取 `encoder_*_read_delta()`（读后清零或记录上次值做差）。
- **内部上拉**：GPIO11/12 同样 `gpio_set_pull_mode(GPIO_PULLUP_ONLY)`（沿用现有模式）。
- **验收**：烧录后 LCD 显示 3 路计数，USB 串口可见日志，GPIO11/12 旋转正常计数。

### 阶段 2：I2C + PCA9535CPW 探测降级 + 按键/SW/INT
- **目标**：PCA9535 未焊接时设备仍正常运行（关键兼容需求）。
- **i2c_io.c**：
  - I2C master 初始化 SDA=GPIO2/SCL=GPIO3，**先启用内部上拉兜底**（`gpio_set_pull_mode(..., GPIO_PULLUP_ONLY)`）。
  - `pca9535_probe()`：写 0x20 检测 ACK；无 ACK → 置 `s_pca9535_present=false`，返回成功（不阻塞启动）。
  - 有 ACK → 写方向寄存器（0x06 发 `0xFF 0xFF` 全输入），开启 GPIO28 下降沿中断（内部上拉兜底）。
  - `pca9535_read_keys()`：仅在 present 时读 Port0/Port1，返回 6 键 + 3 SW 位；未焊时返回空。
- **main.c**：`pca9535_present == false` 时跳过按键/SW 轮询与 INT，日志打印 `pca9535 not found, keys disabled`，**编码器照常工作**。
- **验收**：不焊 PCA9535 烧录运行不崩溃、LCD/编码器正常；焊接后按键/SW/INT 可读。

### 阶段 3：RMT 状态灯
- **目标**：把彩虹循环改为**状态驱动**。
- **state_led.c**：`state_led_set(state)` 映射颜色/效果——`waiting`=绿常亮/慢呼吸、`working`=蓝呼吸、`error/offline`=红闪、`yolo`=紫跑马。
- **main.c**：主循环按当前 vibetty `state`（presence）+ 连接状态驱动灯，去掉固定彩虹。
- **验收**：连接中/已连/断开/working/waiting 五态灯效正确。

### 阶段 4：PDM 双麦录音（仅 RX）
- **目标**：I2S PDM RX，`bclk=GPIO24`、`din=GPIO23`，双麦立体声输入。
- **audio.c**：
  - I2S PDM RX：`I2S_SLOT_MODE_STEREO`（2 麦）+ raw PDM（C5 无硬件 PDM2PCM）。
  - 过采样率 2.048MHz，GDMA 搬运，软件降采样到 16kHz PCM。
  - 提供 `audio_capture_read()` 接口。
- **无 TX 播放**：GPIO15 是 PSRAM CS1 不可用，且全双工 PDM TX 与 RX 时钟冲突（实测 CPU_LOCKUP），故放弃播放输出。
- **验收**：录音 buffer 能取到双麦 raw PDM；无 MIC 时也能初始化不崩溃。

### 阶段 5：Wi-Fi + MQTT + vibetty 协议（核心）
- **目标**：接入 vibetty 0.4.0+，实现发现/显示/输入闭环。
- **wifi_mqtt.c**：
  - Wi-Fi STA（SSID/密码存 NVS），连接失败自动重连。
  - MQTT client（esp-mqtt），broker `mqtt://<PC_IP>:1883`（或外部）。
  - **发现**：订阅 `+/+/+/vibetty` → 收 retained presence → 解析 `{prefix,client_id,ts,title,state,format}`。
  - **订屏**：按 `format` 订 `{p}/screen_text`（`text`，默认）或 `{p}/screen`（JPEG）；发 `sync`（`{"type":"sync","data":{"width":172,"height":320,"pixels":false}}`）拿首帧。
  - **收屏**：`screen_text` 首字节 tag——`0x00`=全屏基线（重置缓冲重放）、`0x01`=增量（追加）；维护简易终端缓冲并渲染到 LCD。`screen`(JPEG) 模式为后续可选（C5 软解 JPEG 成本高，优先 text）。
  - **发输入**：单键 → `{p}/pty_in`（raw 字节）；文本 → `{p}/control`（`input_text` JSON）。
  - **presence 心跳**：`ts` > ~30s 判离线；LWT 空 payload = 实例下线。
- **验收**：PC 上跑 `vibetty -- claude`（`[mqtt] enable=true`），ESP32 发现实例、LCD 显示终端文本、可发方向键/文本并回显。
- **状态（2026-09-06 实测）**：✅ **LCD AI 状态视图 v0.9.6** —— 新增 `lcd_vt`（[lcd_vt.c](main/lcd_vt.c)）：顶部 **彩色状态头**（约 14px：色块 + `BOOT/LINKING/WAITING/WORKING/OFFLINE` 标签 + instance 标题，标题取 presence `title` 并支持 OSC `0;`/`2;` 窗口标题动态覆盖）+ **ANSI 微型终端正文**（28×38 字符网格，5×7 字号 scale1；`LCDVT_COLS/ROWS` 同时作为 sync 报给 vibetty 的 pty 尺寸，使镜像网格与面板精确一致）。正文为 VT100 子集解析器：CSI 光标 `H/f/A/B/C/D/G/d`、擦除 `J/K/X`、行插入/删除 `L/M`、字符插入/删除 `@/P`、SGR 忽略（面板单色渲染）、DEC 私有/alt-screen 忽略、CR/LF/BS/TAB/换行滚动。screen_text **tag0 全屏基线 → `lcd_vt_feed_baseline`**（清屏重放）、**tag1 增量 → `lcd_vt_feed`**（MQTT 任务仅改字符网格+脏行位图，SPI 绘制由主循环 `lcd_vt_poll` 单线程执行）。实测（fake_vibetty 夹具 + broker.emqx.io）：MQTT 连上 → adopt 实例 → 发 `sync {width:28,height:38}` → 收到 `screen_text baseline` 帧 → LCD 显示夹具正文「fake vibetty shell」+ `$ `；编码器方向键实测上行（夹具收到 `pty_in \x1b[C`）。接真实 vibetty 会话后标题/正文即为 agent 实时状态输出。


### 阶段 6：输入映射 + 状态机整合
- **目标**：把物理输入映射成 vibetty keystrokes。
- **input_map.c**：
  - 编码器1 增量 → `pty_in` 方向键/翻页（`\x1b[A/B/C/D`、PageUp/Down）。
  - 编码器2 → 宏/组合键（如 Retry、切模型，发 `input_text` 命令）。
  - 编码器3 → 切换 vibetty 多实例 / 调背光。
  - 按键（PCA9535 存在时）：Accept=`Enter`、Reject=`Esc`、Retry=宏、YOLO=自动接受、ESC=`Esc`、Voice=录音；SW1=`Enter`、SW2=确认、SW3=返回。
  - 文本/命令走 `control` 的 `input_text`；单键/转义序列走 `pty_in`。
- **状态机**：BOOT → WIFI → MQTT → DISCOVER → READY → RECONNECT（参考 vibetty-main 计划的状态机）。
- **验收**：旋转/按键能控制 PC 上的 claude/codex 终端。

### 阶段 7：语音旁路（可选，自定义扩展）
- **目标**：Voice 键 → PDM 采集 → 送自建 PC ASR → 文本回注 vibetty。
- **说明**：vibetty 无音频 topic（0.4.0 已移除服务端 ASR），ESP32-C5 跑不动本地 Whisper，故走**自定义旁路**（非 vibetty 协议）。
- 采集双麦 PCM → 经自定义 MQTT topic 送 PC 端 ASR → 得到文本 → `{p}/control` 的 `input_text` 发回。
- **状态（2026-09-06 实测）**：
  - ✅ PC ASR 服务 `build/vibekey_asr.py`（部署并验证）：MQTT 收 `{p}/audio_pcm` 16k/16bit 单声道帧，Vosk（离线，`vosk-model-small-en-us-0.15`，可换 cn 模型）识别，`stop` 后把文本以 `input_text` 发回 `{p}/control`。实测真实语音样张识别并成功进入 vibetty control。
  - ✅ 固件软件 PDM→16kHz CIC2 降采样 `audio_pdm_decimate()`（无硬件 PDM2PCM）：上电自检合成 440Hz PDM→实测恢复 439Hz OK。
  - ⏳ PCM-over-MQTT 上行 + Voice 键触发：依赖 MIC 与 PCA9535 Voice 键焊接后联调（`audio_pcm` 20ms 帧 + `audio_ctl` start/stop 已定义）。
- 协议：`{p}/audio_ctl`（`{"cmd":"start"|"stop"}`）+ `{p}/audio_pcm`（裸 PCM，qos0 流）。双麦立体声各 16k 先求和为单声道。
- **验收**：语音 → PC 识别 → 文本进入终端（MIC/键到位后整链联调）。

### 阶段 8：健壮性（NVS / OTA / 低功耗）
- **目标**：配置持久化 + 可升级 + 低功耗。
- **nvs_config.c**：Wi-Fi/broker/键位/旋钮映射存 NVS；支持 MQTT 下发配置。
- **OTA**：MQTT 或 HTTP OTA 升级。
- **低功耗**：空闲 Deep-sleep 唤醒。ESP32-C5 仅 **GPIO0~6** 为 RTC IO（实测 `rtc_io_channel.h`），编码器 ENC1/2（GPIO0/1/4/5）可作 EXT1 唤醒；**GPIO28（PCA9535 INT）与 ENC3（GPIO11/12）非 RTC IO，无法从深睡眠唤醒**（已实测确认）。
- **状态（2026-09-06 实测）**：
  - ✅ **8a NVS 配置 + 目标实例持久化**：`nvs_config`（namespace `vkey`：ssid/pass/broker/vprefix）启动时装载，默认 `360WiFi-91868 / broker.emqx.io / ""`。wifi_mqtt 的 SSID/密码/broker 改从 NVS 读取（不再硬编码宏）。发现实例时把**首个** adopted 的 vibetty `prefix` 写入 NVS；重启后只绑定该持久化实例，其它随机 presence（实测 2 个真实公共 vibekeys 实例）一律忽略——修复了阶段 6 复现的「last-wins 多实例劫持」。实测：首启 `(none)→saved`，重启后仅 adopted `root/abc123/999/vibetty`、另两实例被 ignore。
  - ✅ **8b OTA 升级**：自定义分区表（nvs/otadata/phy/ota_0@0x20000/ota_1@0x220000 各 2MB/spiffs），`ota_mqtt` 实现两种传输：(1) **MQTT 分块**：`vkey/<chip>/ota/{ctl,data,status}`，data 帧 = 4B LE offset+2B LE len+payload，worker 任务严格按偏移顺序写 flash、跳过乱序/重复帧，设备在缓冲排空时回报 `written` 由宿主续传（面对公共 broker 的 QoS 抖动可自愈）；(2) **HTTP OTA（实测主通道）**：ctl 带 `url` → 设备 `esp_http_client` 直拉 PC 上 HTTP 服务器固件写 ota_1（实测 1.21MB ~3s，~390KB/s）。实测：v0.9.1(ota_0) 收 MQTT start ctl → HTTP 拉取 v0.9.3 → md5 校验 → `esp_ota_set_boot_partition` → 重启 boot log `Loaded app from partition at offset 0x220000` + `main: firmware v0.9.3`。公共 broker `broker.emqx.io` 的 QoS1/QoS0 长流实测周期性 `Network timeout` 断连（~每 20-40s），故大体积固件走 HTTP 拉取、MQTT 仅作控制面，最稳。
  - ✅ **8c 空闲 Deep-sleep 唤醒**：新增 `power_mgmt` 空闲监视（默认 600s，烧录联调可改 12s）：主循环在编码器变化时 `power_mgmt_mark_activity()`，空闲超时 → deep sleep。**实测关键发现**：EXT1 是电平触发，而这颗编码器在停档位可能让某通道接地为低（实测 gp0/gp1/gp4=0、gp5=1），若直接使能 EXT1 ANY_LOW 会**瞬间重复唤醒**（实测每次 ~16s 即醒、timer 从不触发）；故门控：仅在 4 根唤醒脚**全部为高**时才使能 EXT1（真正转动会拉低唤醒），否则跳过 EXT1 靠定时器唤醒。**定时器(60s)恒使能**作为可靠唤醒兜底 + 定期重连 MQTT/拉取状态。实测（12s 空闲测试版）：`idle -> deep sleep` → USB-Serial/JTAG 掉线 → 定时唤醒重启 log `wake from deep sleep: timer (causes 0x10)` → WiFi/MQTT 自动重连收 screen_text，循环稳定；EXT1 唤醒路径在引脚为低时亦实测会触发。**注意**：深睡眠会断电 USB-Serial/JTAG（COM 口掉线 ~40s 重新枚举），烧录/OTA 工具须在唤醒窗口操作；600s 默认空闲让正常桌面临近使用几乎不睡眠。
- **验收**：断电重启配置不丢；可 OTA；空闲可休眠唤醒。

## 3. 关键风险与约束

1. **GPIO0/1 Strapping**：编码器不得在上电时拉成错误启动电平；PCNT 不会自动配置上下拉，须显式 `gpio_set_pull_mode()`。
2. **GPIO11/12 = UART0**：必须关 UART0 console 改 USB Serial/JTAG（阶段 1）。
3. **GPIO15 = PSRAM CS1**：已实测确认（`MSPI_IOMUX_PIN_NUM_CS1=15`），不可作 GPIO；PDM 播放输出已放弃。
4. **PCA9535 未焊接**：所有路径必须 `probe → 降级`，GPIO2/3/28 内部上拉兜底。
5. **PDM 仅 RX**：无 TX 播放；RX 为 raw 模式（2.048MHz 过采样），需软件降采样。
6. **vibetty 协议**：固件必须 v0.4.0+；默认 `-q text`（`screen_text`）；`--auto-submit` 只对 `input_text` 生效。
7. **工具链**：PowerShell + ESP-IDF 6.0，勿删 `build`，GCC/CMake/Ninja 版本须与缓存一致（见 vibekeyC5.md 第 10 节）。

## 4. 建议实施顺序与里程碑

| 里程碑 | 阶段 | 验收 |
|---|---|---|
| M1 基础外设 | 1–3 | 3 编码器 + 状态灯 + PCA9535 降级运行 |
| M2 音频 | 4 | 双麦录音（RX） |
| M3 接入 vibetty | 5 | 发现/显示/输入闭环 |
| M4 完整控制面 | 6 | 旋钮+按键控制 claude/codex |
| M5 语音（可选） | 7 | 语音→文本进终端 |
| M6 产品化（可选） | 8 | NVS/OTA/低功耗 |

优先打通 **阶段 5（M3）** 的最小闭环：即使按键/语音未接，只要「LCD 显示 AI 状态 + 旋钮发方向键」跑通，VibeKey 核心价值即成立。
