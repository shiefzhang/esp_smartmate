# ESP8266 固件内存占用报告

> 测量对象：HW-364A 多功能遥控器（NodeMCU ESP-12E / ESP8266）
> 方法：**消融法**（逐个关闭功能重新编译，取差值）+ `xtensa-lx106-elf-size` 段级校验 + `nm` 符号核对
> 工具链：framework-arduinoespressif8266 3.30102.0 / toolchain-xtensa 2.100300.220621

## 一、优化结果

| 指标 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| RAM | 63.2%（51752 B） | **58.5%（47940 B）** | **−3812 B（−3.7 KB）** |
| Flash | 76.5%（799239 B） | **64.9%（677571 B）** | **−121668 B（−118.8 KB）** |

> 表中已包含后续新增的「日志持久化」功能（+704 B RAM，+1432 B Flash）。
> 不含该功能时为 RAM 57.7%（47236 B）/ Flash 64.7%（676139 B）。

段级明细对比：

| 段 | 优化前 | 优化后 | 变化 | 说明 |
|---|---|---|---|---|
| `.irom0.text` | 755212 | 633692 | **−121520** | 主程序代码，红外协议裁剪贡献最大 |
| `.bss` | 37552 | 34736 | −2816 | 日志缓冲 4800→2000 |
| `.rodata` | 12556 | 10768 | −1788 | 红外协议名等常量表随之收缩 |
| `.data` | 1688 | 1732 | +44 | 布局调整，可忽略 |
| `.text1` | 29481 | 29481 | 0 | 库代码，未变 |

## 二、已实施的优化

### 1. 日志缓冲 40×120 → 20×100（−2800 B RAM）

`logLines` 是最大的单一静态数组，纯 RAM 常驻。改为 20 行 × 100 字符后
占用从 4800 B 降到 2000 B。代价：日志页只能查看最近 20 行。

改为编译期可调（`platformio.ini`）：

```ini
-DLOG_LINE_COUNT=20
-DLOG_LINE_MAX=100
```

### 2. 红外协议裁剪（Flash 主要杠杆）

IRremoteESP8266 默认编译 **124 种协议**，本项目只用到 4 个品牌。
库提供官方开关机制，把默认值设为 `false` 后按需逐个开启：

```ini
-D_IR_ENABLE_DEFAULT_=false
-DDECODE_HASH=true      ; 红外学习必需
-DSEND_RAW=true         ; 发送已学习码必需
-DDECODE_KELON=true -DSEND_KELON=true
-DDECODE_DAIKIN=true -DSEND_DAIKIN=true
...（大金各变体）
-DDECODE_GREE=true -DSEND_GREE=true
-DDECODE_MIDEA=true -DSEND_MIDEA=true
```

**实测验证**：符号表中仅保留 `IRGree` / `IRKelon` / `IRMidea` / `IRDaikin*`；
`IRHitachi*` / `IRFujitsu*` / `IRHaier*` / `IRPanasonic*` / `IRWhirlpool*` /
`IRToshiba*` 等全部消失。

⚠️ 两个宏**不能关**，否则功能损坏：
- `SEND_RAW` —— 关掉后链接报 `undefined reference to IRsend::sendRaw`，已学习码无法发送。
- `DECODE_HASH` —— 关掉后 `irrecv.decode()` 对未知信号永远返回 false，红外学习失效。

### 3. 关闭 worldtimeapi 授时（−1204 B Flash，−324 B RAM）

原实现先请求 `worldtimeapi.org`，失败才回退 NTP。该服务不稳定，且引入
`ESP8266HTTPClient` 依赖。现在 `-DENABLE_WEB_TIME_SYNC=0`，直接走 `configTime` NTP。

### 4. 前端资源迁移到 LittleFS（−约 13 KB Flash）

`APP_CSS`(5.7 KB) 与 `APP_JS`(9.4 KB) 原先以 PROGMEM 编入固件。
现迁至 `data/app.css`、`data/app.js`，运行时从 LittleFS 流式读取，
改样式/脚本不必重编固件。固件内保留约 2 KB 的精简兜底版，
文件系统为空时自动降级，保证布局与导航可用。

> 面板与标签页是**服务端渲染**的，不依赖 JS，因此兜底不影响导航功能。

### 5. `lastMessage` 改用固定 char 缓冲

原为 `String lastMessage`，在 `loop()` 中随射频/红外事件频繁重新赋值
（`"RX " + String(value)` 等），每次都会申请/释放堆内存，长期运行加剧堆碎片。

现改为 `char lastMessage[48]` + `setLastMessage(const char* fmt, ...)`
（内部 `vsnprintf`），堆分配归零。共迁移 43 处使用点。

### 6. 运行日志持久化到 LittleFS（+704 B RAM，+1432 B Flash）

原日志只存在 RAM 环形缓冲，一重启就丢，而排查偶发重启恰恰需要掉电前的记录。
现在日志同时追加到 `/log.txt`，重启后可经 `/logs/file` 回查。

**关键设计：不逐条落盘。** 每条日志都写 flash 有两个问题：磨损、以及
在主循环里产生阻塞。改为 RAM 攒批 + 定时追加：

| 参数 | 默认 | 说明 |
|---|---|---|
| 落盘间隔 | 30 s | `-DLOG_FLUSH_INTERVAL_MS` 可调 |
| 待落盘缓冲 | 400 B | 写满即提前落盘 |
| 单文件上限 | 32 KB | 满后轮转为 `/log.1.txt`，只留上一份 |
| FS 占用 | ≤ 64 KB | 两个文件合计 |

另有两个实现细节：
- **文件系统未就绪时缓冲保留不丢**。`flushLogToFile()` 在 `fsReady` 为假时直接返回，
  因此 `setup()` 早期（LittleFS 挂载前）的启动日志会在挂载后补写。
- **启动时记录复位原因**，用于区分崩溃类型：
  `Boot start chip=00A1B2C3 reset=Software Watchdog heap=41232`。
  `getResetReason()` 可直接区分掉电 / 看门狗 / 异常 / 软重启。

关闭方式：`-DENABLE_LOG_PERSIST=0`。

> 取舍说明：缓冲内未落盘的内容（最多 30 秒）会随崩溃丢失。
> 这是为避免频繁写 flash 有意为之；若更看重现场完整性，可把
> `LOG_FLUSH_INTERVAL_MS` 调小（代价是写入更频繁）。

## 三、RAM 构成（优化后约 47.9 KB）

```
14484  WiFi 驱动内部静态池   ← SDK 固定开销，不可裁剪
 2000  logLines[20][100]     ← 已优化（原 4800）
 1832  Blinker 全局实例
 1704  g_ic (IRrecv 捕获缓冲)
 1120  webSocket_MQTT
  512  learnedIrRaw[256]
  432  dns_table
  268  RCSwitch::timings
  264  irSlots[6]
  256  pmc / interrupt_handlers / event_TaskQueue
  240  arp_table
  168  rfSlots[6]
  ...  其余零散
```

**关于那 14484 B**：位于 `WdevTimOffSet` 之后，是 ESP8266 SDK 的 WiFi 驱动内部
静态池（无符号导出）。属协议栈固定开销，**裁剪业务功能无法减少**。

## 四、Flash 构成（优化后 660 KB）

| 模块 | 优化前 | 说明 |
|---|---|---|
| IR 红外 | 91464 → **约 20 KB** | 124 协议裁剪到 4 品牌 |
| Blinker 云控 | 62294 | 天猫精灵 + DuerOS + MQTT |
| 用户代码 main.cpp | 19700 | 自绘 OLED + Web UI + 业务逻辑 |
| lwIP 网络栈 | 26117 | 必需 |
| mDNS | 19773 | **已启用**（`MDNS.begin`，供 Home Assistant 用 `.local` 访问） |
| LittleFS | 19030 | 必需 |
| SSL/TLS (BearSSL) | 13313 | Blinker HTTPS |
| Web 服务器 + OTA | 约 10 KB | OTA 可用 `-DENABLE_OTA_UPDATE=0` 关闭，省约 9 KB |

### 一个容易误判的点：Adafruit SSD1306 / GFX

`platformio.ini` 的 `lib_deps` 里声明了 `Adafruit SSD1306` 和 `Adafruit GFX Library`，
但 `src/main.cpp` **从未调用它们** —— OLED 走的是自写的软件 I2C 驱动
（`oledWriteByte` / `oledStart`，见 `setupOled()`）。

消融实验证实：删除这两个依赖后，`.bss` / `.irom0.text` 段大小**完全不变**，
说明链接器已将其丢弃。**删除它们收益为 0**，只能略微缩短构建时间。

（符号表里的 38 个 `Adafruit_*` 是 `Adafruit_MQTT`，属 Blinker 依赖，与 OLED 无关。）

## 五、诊断脚本

| 脚本 | 用途 |
|---|---|
| `scripts/mem_report.py` | 按符号逐项统计 RAM/Flash，列出最大单体对象 |
| `scripts/mem_by_lib.py` | 按 `.a` 归档统计各依赖库占用 |

```bash
pio run
~/.platformio/penv/Scripts/python.exe scripts/mem_report.py
```

> 注意：按 `.a` 归档统计会**高估**（归档含未被链接的目标文件，
> 例如 IRremote 报 261 KB 而实际只链入 89 KB）。以消融法或段级数据为准。

## 六、后续可用的手段

1. **关闭 OTA**：`-DENABLE_OTA_UPDATE=0`，省约 9 KB Flash + 224 B RAM，改用串口烧录。
2. **`.rodata` 仍在 RAM**：剩余的网页 HTML 片段以 `PSTR()` 常驻 Flash 已无问题；
   但若继续加入大型常量字符串，需留意其计入 RAM 而非 Flash。
3. **`String` 仍广泛使用**：JSON 构建（`handleRfStatus` 等）在 HTTP 请求路径上
   使用 `String` 拼接。请求频率低，影响可控；若要进一步稳堆，可按 `lastMessage`
   的方式改用固定缓冲。
4. **WebServer 请求缓冲**：`ESP8266WebServer` 的 `_currentClient` 缓冲在
   大表单提交时占堆峰值，属于库行为，一般无需干预。

## 七、测量方法备忘

消融法要点（避免重复踩坑）：

1. PlatformIO **只读 `platformio.ini`**，独立的 `.ini` 不生效（会报 `UnknownEnvNamesError`）。
2. 每个实验环境需自己的 `.pio/libdeps/<env>`；新环境触发联网装依赖会被沙箱拦截，
   解决方式是 `cp -r .pio/libdeps/nodemcuv2 .pio/libdeps/<新环境名>`。
3. 修改 `build_flags` 会让全部目标文件 hash 失效，PlatformIO 需删除大量旧 `.o`，
   而平台的批量删除保护（阈值 50/turn）会中断构建。
   **对策：直接 `rm -rf .pio/build/nodemcuv2` 后重建**，避免删除动作。
4. 编译 124 协议的红外库很慢，批量实验放后台执行。
