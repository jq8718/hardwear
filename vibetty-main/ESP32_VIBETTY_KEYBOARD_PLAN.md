# ESP32 + vibetty 远端键盘开发计划

## 1. 项目定位

本项目目标是做一个“远端键盘输入设备”，通过 ESP32 连接 MQTT broker，把按键事件发送给 vibetty 服务端，最终由 vibetty 写入共享 PTY，从而控制远端终端（例如 Claude、Codex、shell 等）。

### 1.1 产品目标

- 低成本、可快速原型验证
- 支持 Wi-Fi + MQTT 的远程输入
- 先实现最小可用功能，再扩展为完整外设
- 面向嵌入式工程和硬件原型开发

### 1.2 非目标

- 不做完整 PC 键盘协议栈（如 USB HID）
- 不做高性能多媒体输入
- 不做复杂的图形界面或多通道音视频传输

---

## 2. 系统架构

整体分为 4 层：

1. 硬件层
   - ESP32 主控
   - 按键阵列 / 独立按键
   - OLED / LED / 状态指示
   - USB-C 供电与下载接口

2. 固件层
   - 按键扫描与去抖
   - 按键映射与事件编码
   - Wi-Fi 与 MQTT 连接管理
   - 消息封装与发送

3. 通信层
   - 通过 MQTT broker 与 vibetty 对接
   - 采用 vibetty 的协议格式：`control` 与 `pty_in`

4. 应用层
   - vibetty 作为服务端接收输入
   - 终端应用收到输入后执行

### 2.1 逻辑流程

```text
物理按键 -> 按键扫描 -> 事件编码 -> MQTT 发布 -> vibetty -> PTY 输入
```

---

## 3. 硬件设计方案

### 3.1 推荐平台

优先推荐：
- ESP32-WROOM-32：成本低，成熟稳定
- ESP32-S3：更适合后续扩展 OLED、RGB、更多按键

### 3.2 最小原型硬件清单

| 部件 | 说明 | 推荐数量 |
|---|---|---:|
| ESP32 开发板/模块 | 主控 | 1 |
| 按键开关 | 4~8 个独立按键 | 4~8 |
| 10kΩ 上拉电阻 | 按键输入上拉 | 4~8 |
| LED | 状态指示 | 1 |
| USB-C 接口 | 电源与烧录 | 1 |
| 3.3V 稳压电路 | 如开发板未集成 | 1 |
| 可选 OLED | 显示状态 | 1 |

### 3.3 按键方案建议

#### 方案 A：独立按键（推荐用于 v1.0）

优点：
- 简单，容易调试
- 适合快速验证 MQTT 输入通路

适合键位：
- Enter
- Esc
- Backspace
- Tab
- Arrow Up/Down/Left/Right
- Ctrl / Shift

#### 方案 B：矩阵键盘（推荐用于后续版本）

优点：
- 更像真正的键盘
- 后续扩展性强

缺点：
- 需要更复杂的扫描与去抖处理

### 3.4 推荐原理图结构

建议先做最小板：
- ESP32 模块
- 5V/3.3V 稳压
- USB-UART 下载接口
- 8 个按键接到 GPIO
- 1 个状态 LED
- 1 个复位按钮
- 1 个 BOOT 按钮

### 3.5 GPIO 建议

针对你给定的 ESP32-C5 资源约束，推荐采用下面的引脚分配：

| 功能 | 推荐 GPIO | 说明 |
|---|---:|---|
| 3×3 矩阵键盘 Row0 | GPIO0 | 矩阵行 |
| 3×3 矩阵键盘 Row1 | GPIO1 | 矩阵行 |
| 3×3 矩阵键盘 Row2 | GPIO2 | 矩阵行 |
| 3×3 矩阵键盘 Col0 | GPIO3 | 矩阵列 |
| 3×3 矩阵键盘 Col1 | GPIO4 | 矩阵列 |
| 3×3 矩阵键盘 Col2 | GPIO5 | 矩阵列 |
| I2C 液晶屏 SDA | GPIO6 | LCD 数据线 |
| I2C 液晶屏 SCL | GPIO7 | LCD 时钟线 |
| 编码器 1 A | GPIO8 | 编码器相位 A |
| 编码器 1 B | GPIO9 | 编码器相位 B |
| 编码器 1 SW | GPIO10 | 编码器按键 |
| 编码器 2 A | GPIO23 | 编码器相位 A |
| 编码器 2 B | GPIO24 | 编码器相位 B |
| 编码器 2 SW | GPIO25 | 编码器按键 |
| WS2812 LED 数据线 | GPIO26 | LED 数据输入 |
| 预留扩展 | GPIO27 | 后续扩展按键/状态灯 |
| BOOT 选项按钮 | GPIO28 | 模式/配置按钮 |

### 3.6 资源说明

- GPIO13 / GPIO14 保留给 USB HID 设备，不用于普通输入输出。
- TX0 / RX0 保留给调试和烧录口。
- GPIO0~GPIO10、GPIO23~GPIO28 作为普通 IO 使用。
- 该分配方案可同时支持：
  - 3×3 矩阵键盘
  - I2C 液晶屏
  - 2 个带按键的编码器
  - 1 个内建 WS2812 LED
  - 1 个 BOOT 选项按钮

### 3.7 设计建议

- 矩阵键盘建议使用上拉电阻，并在软件中做消抖。
- I2C 屏幕建议靠近主控，走短线，避免串扰。
- 编码器建议软件中做相位判断和去抖。
- WS2812 数据线尽量短，避免信号衰减。
- GPIO28 作为 BOOT 按钮时，建议默认上拉，按下后触发模式切换。

---

## 4. 固件开发方案

### 4.1 固件架构

建议采用模块化结构：

```text
firmware/
├── main.py
├── wifi_manager.py
├── mqtt_client.py
├── keyboard_map.py
├── protocol.py
├── display.py
└── config.py
```

### 4.2 模块职责

| 模块 | 职责 |
|---|---|
| main.py | 初始化、主循环、状态管理 |
| wifi_manager.py | Wi-Fi 连接、重连 |
| mqtt_client.py | MQTT 连接、订阅、发布 |
| keyboard_map.py | 按键映射与事件编码 |
| protocol.py | 生成 vibetty 兼容消息 |
| display.py | OLED/LED 状态显示 |
| config.py | Wi-Fi/MQTT 参数与按键配置 |

### 4.3 按键处理流程

1. 扫描按钮状态
2. 进行消抖
3. 检测短按 / 长按 / 连击
4. 根据按键映射生成消息
5. 通过 MQTT 发布给 vibetty

### 4.4 状态机设计

建议采用如下状态机：

- BOOT
- WIFI_CONNECTING
- WIFI_CONNECTED
- MQTT_CONNECTING
- MQTT_CONNECTED
- DISCOVERING
- READY
- RECONNECTING

### 4.5 输入编码策略

#### A. 文本输入

用于普通字符、命令、回车。推荐发布到 `control` topic：

```json
{"type":"input_text","data":"ls\n"}
```

#### B. 原始 PTY 输入

用于特殊键，如方向键、Esc、Ctrl+C 等。推荐发布到 `pty_in` topic：

- Arrow Up：`\x1b[A`
- Ctrl+C：`\x03`
- Tab：`\x09`

### 4.6 断线恢复策略

需要实现：
- Wi-Fi 断线自动重连
- MQTT 断线自动重连
- 设备重启后自动恢复
- prefix 丢失时重新订阅 discovery

---

## 5. MQTT 协议对接方案

### 5.1 发现 vibetty 实例

订阅：

```text
+/+/+/vibetty
```

通过保留 presence 消息找到当前可用实例，并解析出 prefix。

### 5.2 发送输入

#### 普通文本输入

- topic：`{prefix}/control`
- payload：

```json
{"type":"input_text","data":"ls\n"}
```

#### 特殊键输入

- topic：`{prefix}/pty_in`
- payload：原始字节序列

### 5.3 推荐按键映射

| 按键 | 发送内容 |
|---|---|
| A | `a` |
| Enter | `\n` |
| Backspace | `\b` |
| Up | `\x1b[A` |
| Down | `\x1b[B` |
| Left | `\x1b[D` |
| Right | `\x1b[C` |
| Ctrl+C | `\x03` |

---

## 6. 工程实施计划

### 阶段 1：原型验证（1~2 周）

目标：
- 用 ESP32 开发板验证 Wi-Fi 和 MQTT
- 接 4~8 个按键
- 完成按键 -> MQTT -> vibetty 的最小闭环

交付物：
- 原型板
- 最小固件
- 可测试的输入发送链路

### 阶段 2：协议联调（2~3 天）

目标：
- 与 vibetty 做端到端联调
- 确认 `control` 与 `pty_in` 都可正确触发

交付物：
- 联调记录
- 修复协议兼容性问题

### 阶段 3：硬件优化（3~5 天）

目标：
- 增加 LED/OLED
- 增加长按、连击、状态显示

交付物：
- 更稳定的外设体验

### 阶段 4：产品化（可选）

目标：
- 做 PCB
- 做外壳
- 做稳定性测试

交付物：
- 可量产的硬件版本

---

## 7. 功能测试计划

### 7.1 基础功能测试

- 按键是否能正确触发
- 是否能发布 MQTT 消息
- 是否能正确连接 Wi-Fi / MQTT
- 是否能发现 vibetty 的 prefix

### 7.2 终端控制测试

- 输入 `ls`
- 输入 `pwd`
- 输入 `echo hello`
- 输入 Enter
- 输入方向键
- 输入 Ctrl+C
- 输入 Backspace

### 7.3 稳定性测试

- 断网后再连
- broker 关闭后再恢复
- 长时间运行 24 小时
- 连续按键 1000 次

### 7.4 兼容性测试

- 不同 ESP32 型号
- 不同 MQTT broker
- 不同 vibetty 配置
- 不同终端（Claude / Codex / shell）

---

## 8. 风险与规避

| 风险 | 影响 | 规避方式 |
|---|---|---|
| MQTT 连接不稳定 | 输入无法送达 | 做重连与状态机 |
| 按键抖动 | 误触发 | 使用去抖和短按/长按分离 |
| GPIO 引脚冲突 | 硬件无法工作 | 参考 ESP32 引脚约束，避开特殊引脚 |
| prefix 未发现 | 消息发错 topic | 先做 discovery，再发送 |
| 固件内存不足 | 运行异常 | 避免过多日志，使用轻量依赖 |

---

## 9. 推荐开发目录结构

```text
esp32_vibetty_keyboard/
├── hardware/
│   ├── schema/
│   ├── pcb/
│   └── bom/
├── firmware/
│   ├── main.py
│   ├── wifi_manager.py
│   ├── mqtt_client.py
│   ├── keyboard_map.py
│   ├── protocol.py
│   ├── display.py
│   └── config.py
├── tests/
│   ├── test_keymap.py
│   ├── test_protocol.py
│   └── test_mqtt.py
└── README.md
```

---

## 10. 里程碑与验收标准

### 里程碑 1：原型完成

验收标准：
- ESP32 可连接 Wi-Fi
- 可连接 MQTT broker
- 可发送至少一种输入消息到 vibetty

### 里程碑 2：功能完整

验收标准：
- 可发送字符、Enter、方向键
- 可成功控制终端

### 里程碑 3：稳定可用

验收标准：
- 断线重连稳定
- 长时间运行无异常
- 可在真实环境下使用

---

## 11. 建议下一步

建议按以下顺序推进：

1. 确定按键布局与最小硬件清单
2. 搭建 ESP32 原型电路
3. 实现 Wi-Fi + MQTT 基础连接
4. 完成 `input_text` 和 `pty_in` 的最小发送逻辑
5. 与 vibetty 做一次端到端联调
6. 再扩展为 OLED / 长按 / PCB 版本

---

## 12. 一句话总结

这是一个“硬件 + 固件 + MQTT + vibetty 协议”组合的嵌入式输入设备项目，最适合先做最小可用原型，再逐步扩展为更完整的远端键盘产品。