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
  ws2812_encoder.c/.h     # 已存在（RMT 编码器）
  + i2c_io.c/.h           # I2C 主机 + PCA9535 探测/读取/优雅降级
  + audio.c/.h            # I2S PDM RX(双麦)/TX(单声道)
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

### 阶段 4：PDM 音频（双麦 RX + 单声道 TX，全双工）
- **目标**：I2S PDM 全双工，`bclk=GPIO24`、`din=GPIO23`（RX 双麦立体声）、`dout=GPIO15`（TX 单声道）。
- **audio.c**：
  - I2S PDM 通道：RX `I2S_SLOT_MODE_STEREO`（2 麦），TX `I2S_SLOT_MODE_MONO`（单声道）。
  - 采样率 16kHz、16bit，GDMA 搬运；RX/TX 同时 enable（全双工）。
  - 提供 `audio_capture_start/read()/stop()` 与 `audio_play(data)` 接口。
- **注意**：GPIO15 需确认未被 PSRAM（SPICS1）占用（vibekeyC5.md 注意事项 3）。
- **验收**：录音 buffer 能取到双麦 PCM；播放能输出单声道 PDM（若暂无功放，先用逻辑分析仪/示波器看 dout 波形）。

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
- **目标**：Voice 键 → PDM 采集 → 送自建 PC ASR（Whisper.cpp）→ 文本回注 vibetty。
- **说明**：vibetty 无音频 topic（0.4.0 已移除服务端 ASR），ESP32-C5 跑不动本地 Whisper，故走**自定义旁路**（非 vibetty 协议）。
- 采集双麦 PCM → 经自定义 MQTT topic / TCP / WebSocket 送 PC 端 Whisper.cpp server → 得到文本 → `{p}/control` 的 `input_text` 发回。
- **验收**：语音 → PC 识别 → 文本进入终端。

### 阶段 8：健壮性（NVS / OTA / 低功耗）
- **目标**：配置持久化 + 可升级 + 低功耗。
- **nvs_config.c**：Wi-Fi/broker/键位/旋钮映射存 NVS；支持 MQTT 下发配置。
- **OTA**：MQTT 或 HTTP OTA 升级。
- **低功耗**：空闲 Deep-sleep，GPIO28（INT）或编码器活动唤醒。
- **验收**：断电重启配置不丢；可 OTA；空闲可休眠唤醒。

## 3. 关键风险与约束

1. **GPIO0/1 Strapping**：编码器不得在上电时拉成错误启动电平；PCNT 不会自动配置上下拉，须显式 `gpio_set_pull_mode()`。
2. **GPIO11/12 = UART0**：必须关 UART0 console 改 USB Serial/JTAG（阶段 1）。
3. **GPIO15 PSRAM**：PDM_DOUT 前确认未接 PSRAM SPICS1；冲突则改 GPIO29。
4. **PCA9535 未焊接**：所有路径必须 `probe → 降级`，GPIO2/3/28 内部上拉兜底。
5. **PDM 单声道**：TX 仅 `I2S_SLOT_MODE_MONO`，无 dout2；RX 双麦仍立体声。
6. **vibetty 协议**：固件必须 v0.4.0+；默认 `-q text`（`screen_text`）；`--auto-submit` 只对 `input_text` 生效。
7. **工具链**：PowerShell + ESP-IDF 6.0，勿删 `build`，GCC/CMake/Ninja 版本须与缓存一致（见 vibekeyC5.md 第 10 节）。

## 4. 建议实施顺序与里程碑

| 里程碑 | 阶段 | 验收 |
|---|---|---|
| M1 基础外设 | 1–3 | 3 编码器 + 状态灯 + PCA9535 降级运行 |
| M2 音频 | 4 | 双麦录音 + 单声道播放 |
| M3 接入 vibetty | 5 | 发现/显示/输入闭环 |
| M4 完整控制面 | 6 | 旋钮+按键控制 claude/codex |
| M5 语音（可选） | 7 | 语音→文本进终端 |
| M6 产品化（可选） | 8 | NVS/OTA/低功耗 |

优先打通 **阶段 5（M3）** 的最小闭环：即使按键/语音未接，只要「LCD 显示 AI 状态 + 旋钮发方向键」跑通，VibeKey 核心价值即成立。
