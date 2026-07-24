#!/usr/bin/env python3
#
# Update the hardcoded allowlist of currently unsupported devices in
# fprint-list-udev-hwdb.c from the libfprint wiki page.

import argparse
import re
import sys
from urllib.request import urlopen

DEFAULT_URL = (
    "https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices.md"
)

ID_RE = re.compile(r"\|.*([0-9a-fA-F]{4}):([0-9a-fA-F]{4}).*\|.*")

BEGIN_MARKER = "  /* --- BEGIN GENERATED IDS --- */\n"
END_MARKER = "  /* --- END GENERATED IDS --- */\n"


def fetch(url):
    with urlopen(url) as response:
        return response.read().decode("utf-8")


def parse_entries(text):
    devices = set()
    for line in text.splitlines():
        match = ID_RE.match(line)
        if match:
            devices.add((match.group(1).lower(), match.group(2).lower()))
    return [f"  {{ .vid = 0x{vid}, .pid = 0x{pid} }}," for vid, pid in sorted(devices)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_file", help="Path to fprint-list-udev-hwdb.c")
    parser.add_argument("--url", default=DEFAULT_URL, help="Wiki page URL")
    args = parser.parse_args()

    entries = parse_entries(fetch(args.url))
    if not entries:
        sys.exit("No device ids found on the wiki page")

    with open(args.source_file, "r", encoding="utf-8") as f:
        content = f.read()

    before, begin, rest = content.partition(BEGIN_MARKER)
    _, end, after = rest.partition(END_MARKER)
    if not begin or not end:
        sys.exit("Could not locate generated id markers in " + args.source_file)

    block = BEGIN_MARKER + "\n".join(entries) + "\n" + END_MARKER
    new_content = before + block + after

    if new_content == content:
        print("allowlist_id_table is already up to date")
        return

    with open(args.source_file, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"Updated allowlist_id_table with {len(entries)} entries")


if __name__ == "__main__":
    main()
