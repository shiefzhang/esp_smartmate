# ESP SmartMate

ESP SmartMate 是基于 ESP8266 的家用遥控网关固件，面向 433MHz 固定码遥控、红外空调遥控、板载 OLED 状态显示和天猫精灵/小度语音触发场景。

目标开发板主要是带 0.96 寸 SSD1306 OLED 的 ESP8266 模块，例如 `HW-364A / ESP8266 0.96 OLED Module V2.1.0`，也兼容常见 NodeMCU ESP-12E 类板卡。

## 功能概览

- 浏览器配网、设备配置、OTA 升级和运行日志查看。
- OLED 自动探测 I2C 总线，显示时间、网络、IP、堆内存、Flash 和最近事件。
- 433MHz 固定码学习、保存、发射、手动录入和闭环自检。
- 红外学习和发射，空调品牌预设可通过编译开关按需启用。
- 天猫精灵 AliGenie 和百度 DuerOS 多路插座接入。
- 433 和红外各 6 个 slot，数据保存到 LittleFS。
- 浏览器日志支持刷新和清除，日志时间使用 OLED 同一套已同步本地时间。
- AliGenie 原始命令、解析结果、拒绝/异常路径和 433 发射耗时都有日志，便于判断语音命令是否真正到达设备。

## 硬件接线

| 模块 | ESP8266 引脚 | 说明 |
|---|---|---|
| 板载 OLED SDA | 自动探测，常见 D2/GPIO4 或 D5/GPIO14 | SSD1306 软件 I2C |
| 板载 OLED SCL | 自动探测，常见 D1/GPIO5 或 D6/GPIO12 | SSD1306 软件 I2C |
| 433 发射 DATA | D7/GPIO13 | FS1000A 等发射模块 |
| 433 接收 DATA | 默认 D2/GPIO4；若与 OLED 冲突会自动改为 D5/GPIO14 | 串口日志会显示最终 RX GPIO |
| 红外发射 S | D0/GPIO16 | HW-477 或红外 LED 发射模块 |
| 红外接收 S | D4/GPIO2 | 独立红外接收头 |
| VCC | 3V 或 5V | 按模块规格选择，ESP8266 GPIO 不能承受 5V 信号 |
| GND | GND | 所有模块必须共地 |

建议给 433 发射模块焊接约 `17.3 cm` 单芯线作为天线。

## 首次配网

1. 烧录固件并启动设备。
2. 如果没有保存过 Wi-Fi，或 Wi-Fi 连接失败，设备会开启配置热点：

   ```text
   SSID: ESP8266-433-Setup
   PASS: 433remote
   IP:   192.168.4.1
   ```

3. 手机或电脑连接该热点。
4. 浏览器打开：

   ```text
   http://192.168.4.1
   ```

5. 在系统页面保存 Wi-Fi、Blinker Key 和其他配置。
6. 连接成功后 OLED 会显示局域网 IP，之后使用该 IP 访问设备页面。

## 网页功能

主页按 tab 流式输出，降低 ESP8266 RAM 压力：

```text
http://设备IP/?tab=system#system
http://设备IP/?tab=logs#logs
http://设备IP/?tab=rf#rf
http://设备IP/?tab=ir#ir
```

主要页面：

- 系统信息：Wi-Fi、IP、时间、内存、Flash、OLED、重启、Wi-Fi 切换、OTA。
- 日志：查看、刷新、清除设备运行日志。
- 天猫精灵：Blinker Key、语音映射说明、433 语音发射 repeat 设置。
- 百度智能屏：DuerOS/小度语音映射说明。
- 433 射频：学习、保存、发射、清除、闭环自检。
- 空调红外：红外学习、保存、发射、清除；空调预设按编译开关启用。

## OLED 显示

常态页面从上到下显示时间、网络/IP、内存和状态信息。事件显示采用行刷新，避免整屏刷新造成闪烁。

- 第 5 行：天猫精灵 AliGenie 或 DuerOS 收到的语音命令。
- 第 6 行：433 或红外实际发射结果，例如 slot、OK、empty。

上电后如果浏览器已经能访问设备页面，OLED 也会根据 Wi-Fi 状态刷新为当前 IP，而不是停留在 setup AP/unset 信息。

## 433 射频

433 码位保存在 LittleFS：

```text
/rf_codes.txt
```

每个 slot 包含：

- `slot`: 1-6
- `name`: 显示名称
- `value`: 433 固定码值
- `bits`: 常见为 24
- `protocol`: rc-switch 协议号，常见为 1
- `pulse`: 脉宽

学习流程：

1. 进入 `433 射频` 页面。
2. 点击开始学习。
3. 在学习窗口内按下 433 遥控器。
4. 收到码后选择 slot 1-6 并保存。
5. 点击对应 slot 的发射按钮测试。

语音触发 433 时使用单独的 repeat 设置，默认值为 12，可在网页中调整。很多 433 固定码设备只有“切换”功能，没有真实的开/关状态，所以天猫精灵的“打开”和“关闭”都会发射同一个保存码；如果 repeat 过大，可能导致被控设备连续识别多次并再次切换，应按实际设备测试调整。

## 红外空调

红外码位保存在 LittleFS：

```text
/ir_codes.txt
```

每个红外 slot 为 1-6。红外支持两种来源：

- 学习码：接收原遥控器 raw 脉冲并保存。
- 预设码：使用 IRremoteESP8266 的空调协议生成命令。

为了降低 RAM 和固件体积，空调预设通过 `platformio.ini` 的编译开关控制：

```ini
build_flags =
  -DENABLE_IR_AC_PRESETS=1
```

设置为 `0` 后重新编译，网页会保留红外学习/发射功能，但隐藏空调品牌预设功能。

## 语音控制

固件通过 Blinker 接入：

- 天猫精灵：AliGenie 多路插座。
- 百度智能屏/小度：DuerOS 多路插座。

语音映射：

| 插座编号 | 动作 |
|---|---|
| 插座 1 | 发送 433 slot 1 |
| 插座 2 | 发送 433 slot 2 |
| 插座 3 | 发送 433 slot 3 |
| 插座 4 | 发送红外 slot 1 |
| 插座 5 | 发送红外 slot 2 |
| 插座 6 | 发送红外 slot 3 |

接入步骤：

1. 在 Blinker App 创建 WiFi 设备。
2. 复制设备 Key。
3. 在设备网页保存 Blinker Key。
4. 重启 ESP8266。
5. 在天猫精灵 App 或小度 App 中绑定 Blinker 账号并同步设备。

## 日志诊断

日志同时输出到串口和网页 `日志` 页面。网页日志支持手动刷新和清除。

AliGenie 相关日志示例：

```text
20:10:01 AliGenie RAW #1 {"powerState":"on","num":1}
20:10:01 AliGenie RX #1 num=1 state=on gap=0ms
20:10:02 RF TX source=AliGenie slot=1 value=14513344 repeat=12 elapsed=376ms
20:10:02 AliGenie done #1 sent=1 elapsed=612ms
```

判断方法：

- 没有 `AliGenie RAW`：设备侧没有收到 Blinker/AliGenie 推来的原始命令，可能是云端、网络或设备离线问题。
- 有 `AliGenie RAW` 但没有 `AliGenie RX`：命令到了设备，但 Blinker 的 AliGenie 解析没有进入 power/query 回调；日志会尝试记录 `AliGenie rejected ... no callback`。
- 有 `AliGenie RX` 但没有 `AliGenie done`：命令处理过程中可能阻塞或异常退出。
- `sent=0`：语音映射到了空 slot，或者对应 433/红外码位未保存。

注意：ESP8266 只有单线程主循环。如果设备正在执行 RF/IR 发射或 OLED/网络处理，新的网络命令必须等 `Blinker.run()` 再次执行后才能被处理；如果云端或 broker 在到达设备前丢弃命令，本机日志不会出现 `RAW`。

## OTA 升级

浏览器打开：

```text
http://设备IP/update
```

默认认证：

```text
username: admin
password: 433remote
```

上传文件：

```text
.pio/build/nodemcuv2/firmware.bin
```

首次建议使用 USB 烧录。确认 OTA 页面可用后，再通过网页升级后续版本。

## 编译和上传

编译：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe run
```

USB 上传：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe run -t upload
```

串口监视：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe device monitor -b 115200
```

## Blinker AliGenie 原始命令日志

Blinker 库没有公开 AliGenie 解析前的回调。项目使用 PlatformIO `extra_scripts` 在编译前给本地依赖中的 `BlinkerApi.h` 注入轻量 hook，用于记录 AliGenie 原始命令和未进入回调的情况：

```text
scripts/patch_blinker.py
```

该脚本是幂等的，PlatformIO 每次编译会自动执行。如果 Blinker 依赖版本内部结构变化导致 hook 找不到，编译会直接报错，避免静默丢失诊断日志。

## 注意事项

- ESP8266 的主要瓶颈是 RAM，不是 Flash。
- LittleFS 首次挂载失败时可能自动格式化，这是正常行为。
- OTA 只做基础认证，建议只在可信局域网使用。
- ESP8266 GPIO 不能直接接 5V 输出信号。
- D4/GPIO2 是启动相关引脚，红外接收模块空闲态应保持高电平。
- 如果频繁出现 OOM，优先关闭空调预设、减少同时打开的网页、检查 heap 是否长期低于 10K。
