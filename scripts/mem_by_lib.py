"""按库归档(.a)精确统计各依赖库对Flash/RAM的实际贡献。

方法：逐个 .a 用 nm -S 读取具名符号大小求和，并按归档归属分类。
ESP8266 上 .rodata / .data / .bss 都占 RAM，Flash 主要是 .text/.irom0.text。
"""
import glob
import os
import subprocess
from collections import defaultdict

TOOL = r"C:\Users\pyrrhus\.platformio\packages\toolchain-xtensa\bin"
NM = os.path.join(TOOL, "xtensa-lx106-elf-nm.exe")
SIZE = os.path.join(TOOL, "xtensa-lx106-elf-size.exe")
BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     ".pio", "build", "nodemcuv2")

# 展示名映射
NICE = {
    "libFrameworkArduino.a": "Arduino 框架核心",
    "libFrameworkArduinoVariant.a": "框架 Variant(NodeMCU)",
    "libBlinker.a": "Blinker 云控(天猫精灵)",
    "libIRremoteESP8266.a": "IRremoteESP8266(红外)",
    "librc-switch.a": "rc-switch(433 射频)",
    "libAdafruit SSD1306.a": "Adafruit SSD1306(OLED)",
    "libAdafruit GFX Library.a": "Adafruit GFX(OLED 绘图)",
    "libAdafruit BusIO.a": "Adafruit BusIO",
    "libESP8266WebServer.a": "ESP8266WebServer",
    "libESP8266httpUpdate.a": "HTTP Update(OTA 升级)",
    "libESP8266HTTPClient.a": "ESP8266HTTPClient",
    "libESP8266WiFi.a": "ESP8266WiFi 驱动",
    "libESP8266mDNS.a": "mDNS",
    "libLittleFS.a": "LittleFS",
    "libEEPROM.a": "EEPROM",
    "libHash.a": "Hash(MD5/SHA,OTA 依赖)",
    "libTicker.a": "Ticker",
    "libWire.a": "Wire(I2C)",
    "libSPI.a": "SPI",
    "libSoftwareSerial.a": "SoftwareSerial",
    "libEthernet.a": "Ethernet(残留)",
}


def analyze(archive):
    """返回 (flash字节, ram字节) —— 基于具名符号。"""
    try:
        out = subprocess.run([NM, "-S", archive], capture_output=True,
                             text=True, timeout=120).stdout
    except Exception:
        return 0, 0
    flash = ram = 0
    for line in out.splitlines():
        p = line.split(None, 3)
        if len(p) != 4:
            continue
        _, size, typ, _name = p
        try:
            size = int(size, 16)
        except ValueError:
            continue
        # a/b/B/c/C: nobits(data段)  d/D/g/G/s/S: 已初始化data(在RAM且占flash镜像)
        if typ in "bB":
            ram += size
        elif typ in "dDgG":
            ram += size
            flash += size
        elif typ in "rR":
            ram += size          # rodata 在 ESP8266 上进 RAM
        elif typ in "tTwW":
            flash += size
    return flash, ram


archives = sorted(glob.glob(os.path.join(BUILD, "lib*", "*.a")))
rows = []
for a in archives:
    f, r = analyze(a)
    base = os.path.basename(a)
    rows.append((NICE.get(base, base), base, f, r))

rows.sort(key=lambda x: -(x[2] + x[3]))

print("=" * 78)
print("各依赖库实际占用（按 .a 归档实测，具名符号口径）")
print("=" * 78)
print(f"{'库':<26}{'Flash':>10}{'RAM':>9}{'合计':>10}   {'占Flash%':>8}")
print("-" * 78)
tf = tr = 0
for nice, base, f, r in rows:
    tf += f
    tr += r
    tot = f + r
    print(f"{nice:<26}{f:>10}{r:>9}{tot:>10}   {f*100/max(tf,1):>7.1f}%")
print("-" * 78)
print(f"{'合计':<26}{tf:>10}{tr:>9}{tf+tr:>10}")

# 固件整体
elf = os.path.join(BUILD, "firmware.elf")
print("\n" + "=" * 78)
print("固件段总量（链接结果）")
print("=" * 78)
out = subprocess.run([SIZE, "-A", elf], capture_output=True, text=True).stdout
segs = {}
for line in out.splitlines():
    p = line.split()
    if len(p) >= 2 and p[0].startswith("."):
        try:
            segs[p[0]] = int(p[1])
        except ValueError:
            pass
ram = segs.get(".bss", 0) + segs.get(".data", 0) + segs.get(".rodata", 0) + segs.get(".noinit", 0)
flash = segs.get(".irom0.text", 0) + segs.get(".text", 0) + segs.get(".text1", 0)
for k in (".data", ".rodata", ".bss", ".noinit", ".text", ".text1", ".irom0.text"):
    if k in segs:
        tag = "  <- RAM" if k in (".bss", ".data", ".rodata", ".noinit") else "  <- Flash"
        print(f"  {k:<14}{segs[k]:>9} B{tag}")
print("-" * 78)
print(f"  {'RAM 合计':<14}{ram:>9} B = {ram/1024:.1f} KB  (ESP8266 可用约 40KB 给用户)")
print(f"  {'Flash 合计':<14}{flash:>9} B = {flash/1024:.1f} KB")
print(f"\n  未被具名符号覆盖的 RAM 缺口: {ram - tr} B")
print(f"  未被具名符号覆盖的 Flash 缺口: {flash - tf} B")
