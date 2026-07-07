from pathlib import Path

Import("env")


header = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "Blinker"
    / "src"
    / "Blinker"
    / "BlinkerApi.h"
)
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
