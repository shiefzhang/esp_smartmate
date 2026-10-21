# ESP SmartMate

ESP SmartMate 是基于 ESP8266 的家用遥控网关固件，面向 433MHz 固定码遥控、红外空调遥控、板载 OLED 状态显示和天猫精灵/小度语音触发场景。

目标开发板主要是带 0.96 寸 SSD1306 OLED 的 ESP8266 模块，例如 `HW-364A / ESP8266 0.96 OLED Module V2.1.0`，也兼容常见 NodeMCU ESP-12E 类板卡。

## 功能概览

- 浏览器配网、设备配置、OTA 升级和运行日志查看。
- OLED 自动探测 I2C 总线，显示时间、网络、IP、堆内存、Flash 和最近事件。
- 433MHz 固定码学习、保存、发射、手动录入和闭环自检。
- 红外学习和发射，空调品牌预设可通过编译开关按需启用。
- 天猫精灵 AliGenie 和百度 DuerOS 多路插座接入。
- Blinker App 手动点按（`btn-light1..3`）同样可触发 433 发射，与语音通道并存。
- 定期上报 Blinker 云端握手状态（`MQTT_CONNECTED` / `MQTT_LOST`），便于分段排查。
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

### 日志持久化（重启后可回查）

日志会另存一份到 LittleFS，**重启或崩溃后仍能看到掉电前的记录**，这对排查
偶发重启最有用。查看方式：日志页面点「已存盘」，或直接访问 `/logs/file`。

写入策略是「RAM 攒批 + 定时追加」，不是每条都写 flash —— 那样既磨损 flash，
又会在主循环里造成阻塞。具体行为：

| 参数 | 默认值 | 说明 |
|---|---|---|
| 落盘间隔 | 30 秒 | 攒够 30 秒或缓冲写满就追加一次 |
| 单文件上限 | 32 KB | 写满后轮转为 `/log.1.txt`，只保留上一份 |
| 常驻 RAM | 400 B | 待落盘缓冲 |
| 文件系统占用 | ≤ 64 KB | 两个文件合计 |

可在 `platformio.ini` 调整：

```ini
-DENABLE_LOG_PERSIST=1          ; 置 0 彻底关闭持久化，省 640 B RAM
-DLOG_FLUSH_INTERVAL_MS=30000   ; 调小=崩溃前丢的日志更少，写入更频繁
```

每次启动会写入分隔行，并带上复位原因，例如：

```text
===== reboot =====
00:00:01 Boot start chip=00A1B2C3 reset=Software Watchdog heap=41232
```

`reset=` 的取值可直接区分问题类型：`Power on`（掉电）、
`Software Watchdog`（主循环被阻塞）、`Exception`（程序异常）、
`External System`（软件调用 `ESP.restart()`）。

> ⚠️ 两点限制：
> 1. 缓冲未落盘的内容（最多 30 秒）会随崩溃丢失，这是为了避免频繁写 flash 的取舍。
> 2. `uploadfs` 会重建整个文件系统，日志文件会一并清空。

### AliGenie 排查

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

上传前端资源到文件系统（**首次烧录或改动 `data/` 下的 app.css / app.js 后必须执行**）：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe run -t uploadfs
```

> ⚠️ `uploadfs` 会用 `data/` 目录重建整个 LittleFS 分区，可能清掉已学习的 433/红外码位。
> 433 码位会从 EEPROM 自动恢复（见 `loadRfSlots()`），红外码位建议先记录。
> 只改固件、不动前端资源时，用 `-t upload` 即可，不会触碰文件系统。

串口监视：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe device monitor -b 115200
```

## 内存与裁剪

ESP8266 只有 80KB DRAM，且 `.rodata` 常量表也计入 RAM。当前占用：

| 指标 | 优化前 | 优化后 |
|---|---|---|
| RAM | 63.2%（51752 B） | **57.7%（47236 B）** |
| Flash | 76.5%（799239 B） | **64.7%（676139 B）** |

各项优化开关集中在 `platformio.ini` 的 `build_flags`，取值含义见文件顶部注释。
完整测量数据与调优方法见 [`docs/memory-report.md`](docs/memory-report.md)。

**红外协议裁剪**是 Flash 压缩的主要手段。IRremoteESP8266 默认编译 124 种协议，
本项目通过 `-D_IR_ENABLE_DEFAULT_=false` 关闭全部，再按品牌逐个开启：

| 界面品牌 | 协议宏 |
|---|---|
| 科龙 / 海信 | `KELON`、`KELON168` |
| 大金 | `DAIKIN` + 各变体（2/64/128/152/160/176/200/216/312） |
| 格力 | `GREE` |
| 美的 / 华凌 / 小米 | `MIDEA`、`MIDEA24` |

新增品牌时，在上表补对应 `DECODE_xxx` / `SEND_xxx` 即可。
两个开关**不可关闭**：`SEND_RAW`（发送已学习的红外码）和
`DECODE_HASH`（学习红外时的兜底解码器，关掉会学不到码）。

## Blinker AliGenie 原始命令日志

Blinker 库没有公开 AliGenie 解析前的回调。项目使用 PlatformIO `extra_scripts` 在编译前给本地依赖中的 `BlinkerApi.h` 注入轻量 hook，用于记录 AliGenie 原始命令和未进入回调的情况：

```text
scripts/patch_blinker.py
```

该脚本是幂等的，PlatformIO 每次编译会自动执行。如果 Blinker 依赖版本内部结构变化导致 hook 找不到，编译会直接报错，避免静默丢失诊断日志。

## 语音链路分段排查

语音控制不通时，按「天猫 → Blinker 云端 → 设备」三段依次确认，不要凭感觉改代码。
固件已内置各段判定日志，配合串口监听即可定位断点。

### 关键日志对照表

| 日志 | 含义 |
| --- | --- |
| `Blinker link MQTT_LOST` / `MQTT_CONNECTED` | 设备与 Blinker 云端的握手状态，10 秒心跳 |
| `MQTT Connected!` / `MQTT Ping!` | 库层 MQTT 握手成功与心跳 |
| `AliGenie RAW #n {...}` | 收到天猫下发的原始报文 |
| `RX any from=X data=...` | 全量下行报文捕获，不限协议，用来区分「云端没发」与「协议不匹配」 |
| `AliGenie rejected RAW #n no callback` | 报文到了但没进回调（类型/字段不匹配） |
| `AliGenie RX #n num=X state=Y` | AliGenie 解析成功并进入映射 |
| `AliGenie num=0 fallback to 433 slot 1` | 天猫按单路下发，已按主灯位兜底 |
| `Blinker data {"btn-light1":"on"}` | Blinker App 手动点按 |
| `Blinker btn-light1 -> 433 slot 1` | App 点按已映射到 433 码位 |
| `RF TX source=... slot=N` | 433 实际发射 |
| `RF TX saved ... slot=N empty` | 该码位没有学习过 |

**一条日志都没有 = 报文根本没到设备**，此时不要怀疑映射逻辑。

### 两条独立的指令通道

固件同时支持两条通道，互不冲突：

1. **天猫 AliGenie**：字段 `pState` + `num`，由 Blinker 库 `aliParse()` 分发到
   `aligeniePowerState()`。映射 1-3 → 433，4-6 → 红外。
2. **Blinker App 手动点按**：字段 `btn-lightN`，走 `attachData` 回调，
   由 `blinkerDataRead()` 解析 `btn-light1..3` 后发射 433。

排查时先确认走的是哪一条：App 点按会打印 `Blinker data`，
天猫语音会打印 `AliGenie RAW`。两者都没有就是云端未下发。

### 常见故障与处理

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| 改名后语音完全无响应 | 设备名不含品类词，天猫无法匹配 | 改成含「灯/插座/插孔」的名字，并重新同步 |
| App 点按无反应 | 组件名不是 `btn-lightN` | 看日志里的真实 key，再补映射 |
| 只有 `MQTT Ping` 没有业务日志 | 设备在线，但云端没转发指令 | 重新绑定/同步平台账号 |
| `invalid num=X` | 天猫设备类型与固件注册类型不符 | 检查是否误改 `BLINKER_ALIGENIE_MULTI_OUTLET` |
| 反复 `MQTT_LOST` | 网络不稳或密钥失效 | 看 auth 是否已配置，必要时重新配密钥 |

### 串口取证方法

设备 IP 不在同网段时，只能靠串口。先开监听再让用户操作：

```bash
# 复位并抓启动日志
pio device monitor -p COM3 -b 115200
```

复位时翻转 DTR/RTS（`rts=True` 拉低 EN）。启动后可从日志直接读出设备 IP，
再用 `http://<ip>/logs` 查看历史环形日志（容量 40 行，会被冲刷，重要日志要及时抓）。

开启 Blinker 库调试可看到完整 MQTT 握手，注意 `BLINKER_DEBUG` 是库内**全局对象**
（`BlinkerDebug.cpp:42`），不能通过 `-D` 定义，只在代码里调用：

```cpp
BLINKER_DEBUG.stream(Serial);   // 默认级别
// BLINKER_DEBUG.debugAll();    // 全量，输出量大慎用
```

`scripts/patch_blinker.py` 注入了两个钩子，编译时自动生效：

- `blinkerInboundAliGenie()` / `blinkerInboundAliGenieParsed()`：包住 `aliParse()`，
  记录天猫原始报文并判断是否进入回调。
- `blinkerInboundAny()`：注入 `BlinkerPROESP.h` 的 `parseData()` 入口，
  **无条件记录所有下行报文**。因为 `aliParse()` 受 `checkAliAvail()` 门控，
  平台未按 AliGenie 协议下发时钩子看不到东西；这个钩子能区分
  「云端根本没发」和「发了但协议不匹配」。

## 接入其他语音平台

天猫精灵与 Blinker 账号关联自 2024 年起仅对高级会员开放，若关联失效导致
语音不通，可改用 HassLife + Home Assistant 绕过：

```text
GET http://esp8266-433.local/send?slot=1&state=on     # 触发 433 码位 1
GET http://esp8266-433.local/rf-state                # 读取各码位状态
GET http://esp8266-433.local/ir-send?slot=1          # 触发红外码位 1
```

设备启用了 mDNS，主机名 **`esp8266-433.local`**，路由器重新分配 IP 也不用改配置。
若环境解析不了 `.local`（Docker 容器常见），可退回用 `http://192.168.71.4`。

完整步骤见 [`docs/hasslife-integration.md`](docs/hasslife-integration.md)。
Home Assistant 的 REST 配置方法见 [`docs/home-assistant-rest.md`](docs/home-assistant-rest.md)。

### 综合面板（设备 + 代理节点）

在树莓派 / Orange Pi 上跑一个轻量反代，把 HA 设备数据和 mihomo 代理节点
合并到同一个网页，并顺带解决两个限制：

- HA 的 REST API 不返回 `Access-Control-Allow-Origin`，浏览器跨域会被拦
- mihomo 启用 `secret` 后，UI 页面拿不到 `Authorization` 头

```
浏览器 → :8090 panel-proxy
           ├─ /ha/*     → HA 8123（服务端注入令牌）
           ├─ /mihomo/* → mihomo 9090（服务端注入 secret）
           └─ /         → 面板页面
```

面板上可看到空调实时功率 / 今日 / 本周 / 本月 / 本年电量，
并可直接点开关控制 433 吸顶灯。详见 [`docs/panel.md`](docs/panel.md)。

| 文件 | 说明 |
| --- | --- |
| [`scripts/panel_proxy.py`](scripts/panel_proxy.py) | 反代服务（纯标准库） |
| [`scripts/panel-proxy.service`](scripts/panel-proxy.service) | systemd 单元 |
| [`mihomo-node-ui.html`](mihomo-node-ui.html) | 面板页面（双入口自适应） |

| 接口 | 作用 |
| --- | --- |
| `GET /send?slot=N&state=on\|off` | 发射第 N 个 433 码位并记录请求状态 |
| `GET /rf-state` | 返回各码位状态 JSON，供外部集成回读 |
| `GET /ir-send?slot=N` | 发射第 N 个红外码位 |
| `GET /logs` | 查看运行日志 |
| `GET /rf-status` | 查询 433 学习状态 |

> 接口执行后返回 `302` 重定向到 `/#rf`，用 `urllib` 调试会看到 `HTTPError 404`，
> 这是重定向导致的**假报错**，实际已发射，以日志 `RF TX source=HTTP` 为准。
> 另注意 433 固定码无法回读灯具真实状态，`/rf-state` 是「最后一次请求的状态」，
> 开和关发的是同一个码；需要真实状态回读请改用继电器方案。

## 注意事项

- ESP8266 的主要瓶颈是 RAM，不是 Flash。
- LittleFS 首次挂载失败时可能自动格式化，这是正常行为。
- OTA 只做基础认证，建议只在可信局域网使用。
- ESP8266 GPIO 不能直接接 5V 输出信号。
- D4/GPIO2 是启动相关引脚，红外接收模块空闲态应保持高电平。
- 如果频繁出现 OOM，优先关闭空调预设、减少同时打开的网页、检查 heap 是否长期低于 10K。
