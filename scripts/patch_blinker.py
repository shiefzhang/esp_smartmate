from pathlib import Path

Import("env")


libdir = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "Blinker"
    / "src"
)

header = libdir / "Blinker" / "BlinkerApi.h"
adapter = libdir / "Adapters" / "BlinkerPROESP.h"

# --- 1. AliGenie 解析前后钩子：记录原始报文 + 是否进入回调 ---
needle = """                            if (BProto::checkAliAvail())
                            {
                                aliParse(BProto::lastRead());
"""
replacement = """                            if (BProto::checkAliAvail())
                            {
                                blinkerInboundAliGenie(BProto::lastRead());
                                aliParse(BProto::lastRead());
                                blinkerInboundAliGenieParsed();
"""
previous_replacement = """                            if (BProto::checkAliAvail())
                            {
                                blinkerInboundAliGenie(BProto::lastRead());
                                aliParse(BProto::lastRead());
"""

if not header.exists():
    raise RuntimeError(f"Blinker header not found: {header}")

source = header.read_text(encoding="utf-8")
if "blinkerInboundAliGenieParsed();" not in source:
    target = previous_replacement if previous_replacement in source else needle
    if target not in source:
        raise RuntimeError("Unsupported Blinker version: AliGenie parse hook not found")
    header.write_text(source.replace(target, replacement, 1), encoding="utf-8")

# --- 2. 全量下行报文钩子：在 parseData() 里无条件记录 fromDevice 与 data ---
# 天猫若未按 AliGenie 协议下发（或被平台降级为普通组件），checkAliAvail() 永远为
# false，上面的钩子看不到任何东西。这里在数据入口无条件打印，用来区分
# "云端根本没发" 与 "发了但协议不匹配"。
data_needle = """            parseData((char *)iotSub_PRO->lastread);
"""
data_replacement = """            blinkerInboundAny((char *)iotSub_PRO->lastread);
            parseData((char *)iotSub_PRO->lastread);
"""

if not adapter.exists():
    raise RuntimeError(f"Blinker adapter not found: {adapter}")

adapter_source = adapter.read_text(encoding="utf-8")
if "blinkerInboundAny(" not in adapter_source:
    if data_needle not in adapter_source:
        raise RuntimeError(
            "Unsupported Blinker version: parseData hook point not found"
        )
    adapter.write_text(
        adapter_source.replace(data_needle, data_replacement, 1), encoding="utf-8"
    )
