"""按功能模块统计 ESP8266 固件的 RAM(BSS/DATA) 与 Flash(text) 占用。"""
import re
import subprocess
import sys
from collections import defaultdict

ELF = sys.argv[1] if len(sys.argv) > 1 else ".pio/build/nodemcuv2/firmware.elf"
NM = r"C:\Users\pyrrhus\.platformio\packages\toolchain-xtensa\bin\xtensa-lx106-elf-nm.exe"

# 顺序敏感：越靠前越具体
RULES = [
    ("IR 红外", r"IRremoteESP8266|IRrecv|IRsend|IRac|IRutils|IRtcp|IRMQTTServer|^send|sendAc|stdAc|irrecv|irsend|IRTimer"),
    ("433 射频", r"RCSwitch|rc_switch"),
    ("Blinker 云控", r"Blinker|blinker|MQTT|webSocket|AliGenie|DuerOS|AsyncClient|ClientAsync"),
    ("OLED 显示", r"Adafruit|GFX|glcdfont|oled|drawPixel|writePixel"),
    ("Web 服务器", r"esp8266webserver|WebServer|RequestHandler|StaticRequestHandler|Uri|FormBodyParser|HTTP_Method"),
    ("OTA 升级", r"Updater|Update\b|md5|MD5|sha1|SHA1"),
    ("SSL/TLS", r"BearSSL|mbedtls|_ssl|tls_|ssl_"),
    ("LittleFS", r"LittleFS|spiffs|Spiffs|fs_|FSImpl|LittleFSImpl"),
    ("HTTP/TCP", r"HTTPClient|http_parser|TCPClient|ClientImpl|BufferSerial|WiFiClient|Client"),
    ("MDNS", r"MDNS|mdns"),
    ("DNS", r"DNSDNS|dns_|Dns|DNS"),
    ("SNTP/时间", r"sntp|SNTP|configTime|time\(|settime|setSyncedUtc"),
    ("EEPROM", r"EEPROM|eeprom_"),
    ("WiFi 驱动", r"esp_wifi|user_init|wifi_|LwipInit|lwipip|wDevCtrl|pktQueue"),
    ("lwIP 网络栈", r"lwip|tcp_|udp_|inet_|netif_|pbuf|netconn|tcpip|ip4_addr"),
    ("新libc/系统", r"^__|libc|newlib|memcpy|memset|strlen|strcpy|printf|_ppfs|_sbrk|_exit|malloc|free|itoa|utoa|dtostr|mult|FPSupport|__div|__udiv|__mod"),
    ("用户代码 main.cpp", r"^(draw|oled|handle|append|save|load|setup|loop|init|sync|send|transmit|parse|html|format|read|write|show|transmit|has|normalized|gpio|is[A-Z]|selected|release|import|detect|select|add|field|extract|setSync)"),
]


def classify(name: str) -> str:
    for label, pat in RULES:
        if re.search(pat, name):
            return label
    return "其他/框架"


def demangle_all(names):
    """批量反解 C++ 符号。"""
    try:
        out = subprocess.run(
            [NM, "-C", "-S", ELF], capture_output=True, text=True, timeout=180
        ).stdout
    except Exception as e:
        print("nm failed:", e, file=sys.stderr)
        return []
    rows = []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4:
            addr, size, typ, name = parts
            try:
                rows.append((typ, int(size, 16), name))
            except ValueError:
                pass
    return rows


rows = demangle_all(None)
print(f"符号总数: {len(rows)}")

RAM = {"b", "B", "d", "D"}          # bss + data
buckets_ram = defaultdict(int)
buckets_flash = defaultdict(int)
detail_ram = []
detail_flash = []

for typ, size, name in rows:
    label = classify(name)
    if typ in RAM:
        buckets_ram[label] += size
        detail_ram.append((size, label, name))
    elif typ in ("t", "T", "r", "R"):
        buckets_flash[label] += size
        detail_flash.append((size, label, name))

# 段总量
seg = subprocess.run(
    [r"C:\Users\pyrrhus\.platformio\packages\toolchain-xtensa\bin\xtensa-lx106-elf-size.exe", "-A", ELF],
    capture_output=True, text=True,
).stdout
seg_tot = {}
for line in seg.splitlines():
    p = line.split()
    if len(p) >= 2 and p[0].startswith("."):
        try:
            seg_tot[p[0]] = int(p[1])
        except ValueError:
            pass

flash_static = seg_tot.get(".irom0.text", 0) + seg_tot.get(".text1", 0) + seg_tot.get(".text", 0)
ram_static = seg_tot.get(".bss", 0) + seg_tot.get(".data", 0) + seg_tot.get(".rodata", 0)

print("\n" + "=" * 64)
print("RAM 静态占用（.bss + .data + .rodata，ESP8266 上 .rodata 也进 RAM）")
print("=" * 64)
print(f"{'模块':<22}{'字节':>10}{'KB':>9}  占比")
ram_total = sum(buckets_ram.values()) or 1
for k, v in sorted(buckets_ram.items(), key=lambda x: -x[1]):
    if v == 0:
        continue
    print(f"{k:<22}{v:>10}{v/1024:>9.2f}  {v*100/ram_total:>5.1f}%")
print("-" * 64)
print(f"{'合计(符号级)':<22}{sum(buckets_ram.values()):>10}{sum(buckets_ram.values())/1024:>9.2f}")
print(f"{'段实际总量':<22}{ram_static:>10}{ram_static/1024:>9.2f}")

print("\n" + "=" * 64)
print("Flash 代码占用（.irom0.text + .text1）")
print("=" * 64)
print(f"{'模块':<22}{'字节':>10}{'KB':>9}  占比")
fl_total = sum(buckets_flash.values()) or 1
for k, v in sorted(buckets_flash.items(), key=lambda x: -x[1]):
    if v == 0:
        continue
    print(f"{k:<22}{v:>10}{v/1024:>9.2f}  {v*100/fl_total:>5.1f}%")
print("-" * 64)
print(f"{'合计(符号级)':<22}{sum(buckets_flash.values()):>10}{sum(buckets_flash.values())/1024:>9.2f}")
print(f"{'段实际总量':<22}{flash_static:>10}{flash_static/1024:>9.2f}")

print("\n" + "=" * 64)
print("RAM 最大单体对象 Top 30")
print("=" * 64)
for size, label, name in sorted(detail_ram, reverse=True)[:30]:
    print(f"{size:>7}  {label:<16} {name[:70]}")

print("\n" + "=" * 64)
print("Flash 最大单体函数 Top 25")
print("=" * 64)
for size, label, name in sorted(detail_flash, reverse=True)[:25]:
    print(f"{size:>7}  {label:<16} {name[:80]}")
