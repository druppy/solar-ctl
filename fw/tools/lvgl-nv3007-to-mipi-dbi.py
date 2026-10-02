#!/usr/bin/env python3
"""Convert LVGL's in-tree NV3007 init sequence to a kernel panel-mipi-dbi
firmware blob (/lib/firmware/panel-mipi-dbi-spi.bin).

The input is LVGL's lv_nv3007.c (v9.4.0+). We parse it directly so the blob
stays in sync with whatever LVGL version we pin: re-run this tool after bumping
LVGL and review the --dump output.

Firmware format (authoritative source: drivers/gpu/drm/tiny/panel-mipi-dbi.c
in our 6.18.33 kernel; also github.com/notro/panel-mipi-dbi wiki):

    15 bytes magic  'MIPI DBI' + 7 NUL
     1 byte         file_format_version = 1
     n bytes       commands: cmd, num_params, [params...]
                  delay: NOP (0x00) with exactly one param = milliseconds (u8)

LVGL list format (lv_lcd_generic_mipi.c :: lv_lcd_generic_mipi_send_cmd_list):

    cmd, num_params, [params...]
    0xFF (LV_LCD_CMD_DELAY_MS), N  -> delay N * 10 ms   (note: 10 ms units!)
    0xFF, 0xFF (LV_LCD_CMD_EOF)    -> end of list

Assembly mirrors lv_nv3007_create(): page-select 0xFF <p0>, init_cmd_list,
page-select 0xFF <p1>, init_cmd_list_2. The 0xFF page-select values are parsed
out of lv_nv3007_create() itself, not hardcoded.

Usage:
    ./lvgl-nv3007-to-mipi-dbi.py --src lv_nv3007.c [--dump] [-o out.bin]
"""

import argparse
import re
import struct
import sys

MAGIC = b"MIPI DBI" + b"\x00" * 7
VERSION = 1
DELAY_MS = 0xFF  # LV_LCD_CMD_DELAY_MS == LV_LCD_CMD_EOF == 0xFF in LVGL 9.4
EOF = 0xFF


def strip_c_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def parse_defines(src: str) -> dict:
    """#define NAME value (hex or dec ints only) from the driver source."""
    defines = {}
    for m in re.finditer(r"#define\s+(\w+)\s+\(?(0x[0-9A-Fa-f]+|\d+)\)?\s*$", src, re.M):
        defines[m.group(1)] = int(m.group(2), 0)
    # Cross-check the header constants (defined in lv_lcd_generic_mipi.h,
    # not in lv_nv3007.c): both must be 0xFF for the parser logic to hold.
    for name, expect in (("LV_LCD_CMD_DELAY_MS", 0xFF), ("LV_LCD_CMD_EOF", 0xFF)):
        val = defines.get(name, expect)
        if val != expect:
            sys.exit(f"error: {name} = {val:#x}, expected {expect:#x} — parser needs review")
        defines.setdefault(name, expect)
    return defines


def parse_page_selects(src: str) -> list:
    """The two 0xFF page-select writes done by lv_nv3007_create(), in order."""
    compact = re.sub(r"\s+", "", src)
    pages = re.findall(r"0xFF\},1,\(constuint8_t\[\]\)\{(0x[0-9A-Fa-f]{2})\},1", compact)
    if len(pages) != 2:
        sys.exit(f"error: found {len(pages)} page-select writes in create(), expected 2 — source changed?")
    return [int(p, 0) for p in pages]


def parse_array(src: str, defines: dict, name: str) -> list:
    """Return list of ('cmd', cmd, [params]) / ('delay', ms) tuples."""
    m = re.search(r"static const uint8_t " + name + r"\s*\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit(f"error: array {name}[] not found — LVGL source layout changed?")
    tokens = []
    for tok in m.group(1).split(","):
        tok = tok.strip()
        if not tok:
            continue
        if tok.startswith("0x") or tok.isdigit():
            tokens.append(int(tok, 0))
        elif tok in defines:
            tokens.append(defines[tok])
        else:
            sys.exit(f"error: unknown token {tok!r} in {name}[] — LVGL source changed?")
    entries, i = [], 0
    while i < len(tokens):
        cmd, num = tokens[i], tokens[i + 1]
        i += 2
        if cmd == DELAY_MS:
            if num == EOF:
                break  # end of list (a trailing DELAY_MS,EOF is the LVGL terminator)
            ms = num * 10  # LVGL delays are in 10 ms units
            if ms > 255:
                sys.exit(f"error: delay {ms} ms exceeds kernel u8 field")
            entries.append(("delay", ms))
        else:
            params = tokens[i : i + num]
            if len(params) != num:
                sys.exit(f"error: truncated params for cmd {cmd:#02x} in {name}[]")
            i += num
            entries.append(("cmd", cmd, params))
    return entries


def parse_page_selects(src: str) -> list:
    """Extract the 0xFF page-select param values from lv_nv3007_create()."""
    compact = re.sub(r"\s+", "", src)
    vals = re.findall(r"0xFF\},1,\(constuint8_t\[\]\)\{(0x[0-9A-Fa-f]{2})\},1", compact)
    if len(vals) != 2:
        sys.exit(f"error: expected 2 page-select sends in lv_nv3007_create(), found {len(vals)}")
    return [int(v, 0) for v in vals]


def build_commands(entries_per_section) -> bytes:
    buf = bytearray()
    for kind, *rest in entries_per_section:
        if kind == "delay":
            buf += bytes([0x00, 0x01, rest[0]])  # NOP + ms
        else:
            cmd, params = rest
            buf += bytes([cmd, len(params)]) + bytes(params)
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", required=True, help="path to LVGL lv_nv3007.c")
    ap.add_argument("-o", "--output", default="panel-mipi-dbi-spi.bin")
    ap.add_argument("--dump", action="store_true", help="print parsed sequence, write nothing")
    args = ap.parse_args()

    raw = open(args.src).read()
    src = strip_c_comments(raw)
    defines = parse_defines(src)
    p0, p1 = parse_page_selects(raw)
    if (p0, p1) != (0xA5, 0x00):
        print(f"warning: page-select values are {p0:#04x}/{p1:#04x}, not 0xa5/0x00 — review", file=sys.stderr)
    l1 = parse_array(src, defines, "init_cmd_list")
    l2 = parse_array(src, defines, "init_cmd_list_2")

    seq = [("cmd", 0xFF, [p0])] + l1 + [("cmd", 0xFF, [p1])] + l2

    if args.dump:
        for e in seq:
            if e[0] == "delay":
                print(f"delay {e[1]}")
            else:
                print(f"command 0x{e[1]:02x}" + "".join(f" 0x{p:02x}" for p in e[2]))
        n_cmd = sum(1 for e in seq if e[0] == "cmd")
        print(f"# {n_cmd} commands, {sum(1 for e in seq if e[0]=='delay')} delays", file=sys.stderr)
        return

    blob = MAGIC + struct.pack("B", VERSION) + build_commands(seq)
    open(args.output, "wb").write(blob)
    print(f"wrote {args.output}: {len(blob)} bytes, {len(seq)} ops")


if __name__ == "__main__":
    main()
