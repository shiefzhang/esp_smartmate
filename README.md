# ESP SmartMate

ESP SmartMate 是一个基于 ESP8266 的 433MHz 智能遥控网关。它提供浏览器配网、OLED 状态显示、433 固定码学习/发射、天猫精灵语音触发、LittleFS 码库持久化和网页 OTA 升级能力。

当前固件主要针对 `HW-364A / ESP8266 0.96 OLED Module V2.1.0` 这类带板载 0.96 英寸 SSD1306 单色 OLED 的 ESP8266 开发板。

## 功能

- 首次启动或 Wi-Fi 连接失败时自动开启配置热点。
- 浏览器访问设备网页，选择附近 Wi-Fi 并保存密码。
- OLED 显示本地时间、SSID、IP、433 接收码、系统状态。
- 本地时间优先通过 `worldtimeapi.org` 同步，失败后自动使用 NTP 兜底。
- 433MHz 固定码学习、保存、发送和手动录入。
- 通过 LittleFS 保存 12 路 433 码位。
- 天猫精灵通过 Blinker/AliGenie 触发 433 命令。
- 网页 OTA 更新固件。
- 浏览器状态页支持局域网内其他电脑或手机访问。

## 硬件

| 模块 | ESP8266 引脚 |
|---|---|
| 板载 OLED DATA | D5 / GPIO14 |
| 板载 OLED CLOCK | D6 / GPIO12 |
| 433 发射模块 DATA | D7 / GPIO13 |
| 433 接收模块 DATA | D2 / GPIO4 |
| 433 模块 VCC | 3V 或 5V，按模块规格选择 |
| GND | 共地 |

常见 433 模块识别：

- `FS1000A` 小板通常是发射端。
- 带可调电感、芯片和较长 PCB 的模块通常是接收端。

建议给 433 发射模块焊接约 `17.3 cm` 单芯线作为天线。433 模块和 ESP8266 必须共地。

## OLED 显示

OLED 使用 SSD1306 软件 I2C 驱动：

- Clock: D6 / GPIO12
- Data: D5 / GPIO14

联网后顶部显示本地时间，例如：

```text
2026-06-14 SUN 22:30
```

中间区域显示网络和 433 状态。底部每 15 秒轮播两屏系统数据：

```text
HEAP 38K FRAG 3%
CPU 80M RSSI -55 CH 6
```

```text
FLASH 4096K FW 605K
FREE 2500K ID xxxxxx
```

OLED 已改为局部刷新：时间、状态区、系统数据分别刷新，减少全屏闪烁。

## 首次配网

1. 烧录固件并启动设备。
2. 如果没有保存过 Wi-Fi，或 15 秒内连接失败，设备会开启热点：

   ```text
   SSID: ESP8266-433-Setup
   PASS: 433remote
   IP:   192.168.4.1
   ```

3. 手机或电脑连接这个热点。
4. 浏览器访问：

   ```text
   http://192.168.4.1
   ```

5. 在网页中选择 Wi-Fi，输入密码并保存。
6. 连接成功后，OLED 会显示当前 SSID 和设备 IP。
7. 同一局域网内的电脑或手机可以访问 OLED 上显示的 IP。

## 网页功能

设备主页提供：

- Wi-Fi、IP、LittleFS、OTA、语音状态。
- 最近一次接收的 433 码。
- 12 路 433 码位管理。
- 每路码位可以学习、发送、清除。
- 手动录入 `value`、`bits`、`protocol`、`pulse`。
- 433 发射测试，不保存，直接发送一次。
- 进入 Wi-Fi 配置、天猫精灵配置和 OTA 更新页面。

## 433 码位管理

固件使用 LittleFS 保存 12 路码位，路径为：

```text
/rf_codes.txt
```

第一次启动时，如果 LittleFS 中没有码库文件，会把旧 EEPROM 中的 3 个码位导入到 slot 1-3。

码位字段：

- `slot`: 1-12
- `name`: 显示名称，最多 16 字符
- `value`: 433 固定码值
- `bits`: 常见为 24
- `protocol`: rc-switch 协议号，常见为 1
- `pulse`: 脉宽，常见为 350

学习流程：

1. 在网页点击某个 slot 的 `Learn`。
2. 15 秒内按下 433 遥控器按钮。
3. 设备收到固定码后自动保存到该 slot。

## 天猫精灵

天猫精灵接入通过 Blinker 的 AliGenie 能力实现。

1. 在 Blinker App 创建 WiFi 设备。
2. 复制设备密钥。
3. 打开设备网页，进入 `Voice` 页面。
4. 填入 Blinker 设备密钥并保存。
5. 重启 ESP8266。
6. 在 Blinker App 中确认设备在线，并绑定天猫精灵。

当前语音映射：

| 天猫精灵多路插座 | 433 码位 |
|---|---|
| 插座 1 | slot 1 |
| 插座 2 | slot 2 |
| 插座 3 | slot 3 |

433 固定码通常只有一个触发码，没有真实开关状态反馈，所以“打开”和“关闭”都会发送同一个保存码。

## 网页 OTA

固件支持浏览器 OTA 升级：

```text
http://设备IP/update
```

认证信息：

```text
username: admin
password: 433remote
```

上传文件：

```text
.pio/build/nodemcuv2/firmware.bin
```

建议第一次仍使用 USB 烧录。成功运行带 OTA 的版本后，后续再通过网页升级。

## 编译与烧录

项目使用 PlatformIO。

```powershell
pio run
pio run -t upload
```

如果 `pio` 不在 PATH 中，可以使用 PlatformIO 虚拟环境入口：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe run -t upload
```

如果依赖清理过程很慢，可以使用：

```powershell
C:\Users\pyrrhus\.platformio\penv\Scripts\pio.exe run --disable-auto-clean
```

串口监视器：

```powershell
pio device monitor -b 115200
```

## 上传失败处理

如果看到：

```text
Failed to connect to ESP8266: Timed out waiting for packet header
```

可以按顺序检查：

- USB 线是否支持数据传输。
- Windows 设备管理器中是否出现 CH340/USB Serial COM 口。
- PlatformIO 上传端口是否选中真实 COM 口，而不是 COM1。
- 上传时按住 `BOOT/FLASH`，开始连接后松开。
- 重新插拔开发板后再上传。

## 时间同步

设备联网后每小时同步一次时间。

同步顺序：

1. `http://worldtimeapi.org/api/timezone/Asia/Shanghai`
2. NTP 兜底：
   - `ntp.aliyun.com`
   - `pool.ntp.org`
   - `time.windows.com`

如果 worldtimeapi 返回 `HTTP code: -5`，通常表示连接中途断开，固件会自动尝试 NTP。

## 注意事项

- 433 固定码没有可靠的状态反馈，网页状态只代表最近接收或发送的码值。
- LittleFS 首次挂载失败时会自动格式化，这是正常行为。
- OTA 页面只做了基础认证，建议只在可信局域网使用。
- ESP8266 的主要瓶颈是 RAM，不是 Flash。避免在网页里加入过大的图片或复杂前端框架。
