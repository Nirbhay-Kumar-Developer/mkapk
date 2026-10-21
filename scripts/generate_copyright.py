#!/usr/bin/env python3
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
DEB_ROOT = ROOT_DIR / "mkapk-aarch64"
TARGET_COPYRIGHT = DEB_ROOT / "data/data/com.termux/files/usr/share/doc/mkapk/copyright"
THIRD_PARTY_DIR = ROOT_DIR / "THIRD-PARTY-LICENSES"
MAIN_LICENSE = ROOT_DIR / "LICENSE"
MAVEN_THIRD_PARTY = ROOT_DIR / "config/target/generated-sources/license/THIRD-PARTY.txt"

# Ensure target directories exist
THIRD_PARTY_DIR.mkdir(parents=True, exist_ok=True)
TARGET_COPYRIGHT.parent.mkdir(parents=True, exist_ok=True)


def format_indented(text: str) -> str:
    """Formats raw text into standard DEP-5 indented license blocks."""
    lines = text.splitlines()
    while lines and (not lines[0].strip() or lines[0].strip().startswith("#")):
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(f" {line.rstrip()}" if line.strip() else " ." for line in lines)


# 1. READ ALL FILES IN THIRD-PARTY-LICENSES/ BLINDLY
third_party_licenses = {}
if THIRD_PARTY_DIR.exists():
    for item in sorted(THIRD_PARTY_DIR.iterdir()):
        if item.is_file():
            # Blindly read every file present in the directory
            third_party_licenses[item.name] = item.read_text(errors="ignore")

maven_lines = []
if MAVEN_THIRD_PARTY.exists():
    for line in MAVEN_THIRD_PARTY.read_text().splitlines():
        s = line.strip()
        if s and not s.lower().startswith("lists of "):
            maven_lines.append(s)

# Extra bundled tool dependency to append
andresguard_entry = "(Apache License, Version 2.0) AndResGuard CLI (com.tencent.mm.resourceproguard.cli-1.2.15 - https://github.com/shwenzhang/AndResGuard/)"


# 2. ASSEMBLE DEP-5 DEBIAN COPYRIGHT FILE BY APPENDING TEXT SECTIONS
sections = []

sections.append("""Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: mkapk
Upstream-Contact: Nirbhay Kumar <https://github.com/Nirbhay-Kumar-Developer/mkapk>
Source: https://github.com/Nirbhay-Kumar-Developer/mkapk""")

sections.append("""Files: *
Copyright: 2026 Nirbhay Kumar
License: mkapk License""")

# Append Main Project License Text
if MAIN_LICENSE.exists():
    sections.append(f"License: mkapk-Custom-License\n{format_indented(MAIN_LICENSE.read_text())}")

# Append each file found in THIRD-PARTY-LICENSES/ blindly
for name, content in third_party_licenses.items():
    sections.append(f"License: {name}\n{format_indented(content)}")

# Append Maven and extra runtime tool dependency notices
if maven_lines:
    maven_formatted = "\n".join(f"  {ml}" for ml in maven_lines)
    sections.append(f"""Files: data/data/com.termux/files/usr/share/mkapk/lib/*
Copyright: Various upstream authors
License: Listed below
Comment:
{maven_formatted}
  {andresguard_entry}""")
else:
    sections.append(f"""Files: data/data/com.termux/files/usr/share/mkapk/tools/*
Copyright: Tencent / WeChat Teams & shwenzhang
License: Apache-2.0
Comment:
  {andresguard_entry}""")

TARGET_COPYRIGHT.write_text("\n\n".join(sections) + "\n")
print(f">> [COPYRIGHT] Generated DEP-5 copyright at: {TARGET_COPYRIGHT}")
