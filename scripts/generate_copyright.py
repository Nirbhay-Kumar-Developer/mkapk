#!/usr/bin/env python3
from pathlib import Path
import shutil
import zipfile

ROOT_DIR = Path(__file__).resolve().parent.parent
DEB_ROOT = ROOT_DIR / "mkapk-aarch64"
DOC_DIR = DEB_ROOT / "data/data/com.termux/files/usr/share/doc/mkapk"
TARGET_COPYRIGHT = DOC_DIR / "copyright"
TARGET_LICENSE = DOC_DIR / "LICENSE"
TARGET_NOTICE = DOC_DIR / "NOTICE"
TARGET_THIRD_PARTY_DIR = DOC_DIR / "THIRD-PARTY-LICENSES"

THIRD_PARTY_DIR = ROOT_DIR / "THIRD-PARTY-LICENSES"
MAIN_LICENSE = ROOT_DIR / "LICENSE"
MAVEN_THIRD_PARTY = ROOT_DIR / "config/target/generated-sources/license/THIRD-PARTY.txt"
DEPENDENCIES_DIR = ROOT_DIR / "config/target/dependencies"
LIBS_DIR = ROOT_DIR / "libs"

# Ensure target directories exist
DOC_DIR.mkdir(parents=True, exist_ok=True)
TARGET_THIRD_PARTY_DIR.mkdir(parents=True, exist_ok=True)

# 1. Copy full LICENSE and THIRD-PARTY-LICENSES/ files into usr/share/doc/mkapk/
if MAIN_LICENSE.exists():
    shutil.copy2(MAIN_LICENSE, TARGET_LICENSE)
    print(f">> [COPYRIGHT] Copied LICENSE to {TARGET_LICENSE}")

if THIRD_PARTY_DIR.exists():
    for item in THIRD_PARTY_DIR.iterdir():
        if item.is_file():
            shutil.copy2(item, TARGET_THIRD_PARTY_DIR / item.name)
    print(f">> [COPYRIGHT] Synced THIRD-PARTY-LICENSES to {TARGET_THIRD_PARTY_DIR}")

# 2. Dynamically extract, clean, and deduplicate NOTICE files from JARs
extracted_notices = {}
search_paths = [DEPENDENCIES_DIR, LIBS_DIR]

for search_path in search_paths:
    if search_path.exists():
        for jar_path in sorted(search_path.glob("**/*.jar")):
            try:
                with zipfile.ZipFile(jar_path, 'r') as z:
                    for filename in z.namelist():
                        if "NOTICE" in filename.upper():
                            content = z.read(filename).decode('utf-8', errors='ignore').strip()
                            if content:
                                normalized_content = "\n".join(line.strip() for line in content.splitlines() if line.strip())
                                if normalized_content not in extracted_notices:
                                    extracted_notices[normalized_content] = []
                                extracted_notices[normalized_content].append(jar_path.name)
            except Exception as e:
                print(f">> [WARNING] Failed to inspect JAR {jar_path.name}: {e}")

notice_sections = [
    "mkapk includes third-party software and components subject to their respective upstream NOTICE files:",
    "=" * 79 + "\n"
]

for content, jars in extracted_notices.items():
    jar_list_str = ", ".join(sorted(jars))
    notice_sections.append(
        f"--------------------------------------------------------------------------------\n"
        f"Bundled JAR(s): {jar_list_str}\n"
        f"--------------------------------------------------------------------------------\n"
        f"{content}\n"
    )

TARGET_NOTICE.write_text("\n".join(notice_sections))
print(f">> [COPYRIGHT] Generated dynamic combined NOTICE at: {TARGET_NOTICE}")

# 3. Parse Maven dependencies for DEP-5 Comment block (PURELY Java libraries)
maven_lines = []
if MAVEN_THIRD_PARTY.exists():
    for line in MAVEN_THIRD_PARTY.read_text().splitlines():
        s = line.strip()
        if s and not s.lower().startswith("lists of "):
            maven_lines.append(f"  {s}")

andresguard_entry = "  (Apache-2.0) AndResGuard CLI (com.tencent.mm.resourceproguard.cli-1.2.15)"

# 4. Assemble clean DEP-5 copyright file
sections = []

# Header stanza
sections.append("""Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: mkapk
Upstream-Contact: Nirbhay Kumar <https://github.com/Nirbhay-Kumar-Developer/mkapk>
Source: https://github.com/Nirbhay-Kumar-Developer/mkapk""")

# Main binary & coordinator JAR stanza
sections.append("""Files: bin/mkapk
       share/mkapk/mkapk-coordinator.jar
Copyright: 2026 Nirbhay Kumar
License: LicenseRef-mkapk-Source-Available
 Full license text is located at:
 /data/data/com.termux/files/usr/share/doc/mkapk/LICENSE
 .
 Statically embeds header-only routines from nlohmann-json (MIT) and
 xxHash (BSD-2-Clause). Full third-party license texts are located in:
 /data/data/com.termux/files/usr/share/doc/mkapk/THIRD-PARTY-LICENSES/""")

# Third-party Java libraries stanza (Maven only)
comment_block = "\n".join(maven_lines) if maven_lines else "  None"
sections.append(f"""Files: share/mkapk/lib/*
Copyright: Various upstream authors
License: Apache-2.0 and MIT and EPL-2.0
 Full license texts are located in:
 /data/data/com.termux/files/usr/share/doc/mkapk/THIRD-PARTY-LICENSES/
Comment:
{comment_block}""")

# Tools stanza (AndResGuard only)
sections.append(f"""Files: share/mkapk/tools/*
Copyright: Tencent / WeChat Teams & shwenzhang
License: Apache-2.0
 Full license text is located at:
 /data/data/com.termux/files/usr/share/doc/mkapk/THIRD-PARTY-LICENSES/APACHE-LICENSE-2.0
Comment:
{andresguard_entry}""")

TARGET_COPYRIGHT.write_text("\n\n".join(sections) + "\n")
print(f">> [COPYRIGHT] Generated modular DEP-5 copyright at: {TARGET_COPYRIGHT}")
