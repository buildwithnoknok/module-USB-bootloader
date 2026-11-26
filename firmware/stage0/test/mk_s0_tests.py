#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
"""
mk_s0_tests.py - SWD test images for the frozen CH32V203 stage-0 (layout 4, DEV-237).

Each image is a full 32 KB flash state that stage-0 meets at the next reset. Flash it
with `minichlink -w <img> flash -b`, wait ~2 s, then read back over SWD and compare with
the expectation this script prints (run_s0_tests.sh does all of it).

  t_normal     stage-0 + stage-1 A + app + metadata              -> stage-1 A kept, app runs
  t_update     stage-1 A, staging = stage-1 B, control block ok   -> stage-1 region == B, ctrl blank
  t_midcopy    as t_update, but stage-1 region already HALF B and
               half garbage (= a power cut in the middle of the
               copy)                                              -> region == B, ctrl blank
  t_baddesc    as t_update, descriptor CRC wrong                  -> region == A, ctrl untouched
  t_badimg     as t_update, staged image CRC wrong                -> region == A, ctrl untouched
  t_badgeo     as t_update, stage1_len larger than the region     -> region == A, ctrl untouched

Usage: python3 mk_s0_tests.py <stage0.bin> <s1_A.bin> <s1_B.bin> <app.bin> <outdir>
"""
import os
import struct
import sys
import zlib

FLASH, S1, APP, CTRL, META = 0x8000, 0x0400, 0x2000, 0x7E00, 0x7F00
CTRL_MAGIC, META_MAGIC = 0x6E6B5530, 0x6E6B5542
FLASH_BASE = 0x08000000


def crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def base(s0, s1):
    img = bytearray(b"\xff" * FLASH)
    img[0:len(s0)] = s0
    img[S1:S1 + len(s1)] = s1
    return img


def with_app(img, app):
    img[APP:APP + len(app)] = app
    img[META:META + 12] = struct.pack("<III", META_MAGIC, len(app), crc(app))
    return img


def armed(img, s1b, desc_ok=True, img_crc=None, length=None):
    img[APP:APP + len(s1b)] = s1b
    ln = len(s1b) if length is None else length
    words = [CTRL_MAGIC, FLASH_BASE + APP, ln, crc(s1b) if img_crc is None else img_crc, FLASH_BASE + APP]
    d = struct.pack("<5I", *words)
    dc = crc(d) ^ (0 if desc_ok else 0xDEAD)
    img[CTRL:CTRL + 24] = d + struct.pack("<I", dc)
    return img


def main(a):
    s0, s1a, s1b, app = (open(f, "rb").read() for f in a[:4])
    out = a[4]
    os.makedirs(out, exist_ok=True)
    region = APP - S1
    exp_b = s1b + b"\xff" * (region - len(s1b))
    exp_a = s1a + b"\xff" * (region - len(s1a))
    tests = {}
    tests["t_normal"] = (with_app(base(s0, s1a), app), exp_a, "blank")
    tests["t_update"] = (armed(base(s0, s1a), s1b), exp_b, "blank")
    mid = base(s0, s1a)
    half = (len(s1b) // 2) & ~0xFF
    mid[S1:S1 + half] = s1b[:half]
    mid[S1 + half:S1 + half + 512] = bytes((i * 37) & 0xFF for i in range(512))   # torn page
    tests["t_midcopy"] = (armed(mid, s1b), exp_b, "blank")
    tests["t_baddesc"] = (armed(base(s0, s1a), s1b, desc_ok=False), exp_a, "set")
    tests["t_badimg"] = (armed(base(s0, s1a), s1b, img_crc=0x12345678), exp_a, "set")
    tests["t_badgeo"] = (armed(base(s0, s1a), s1b, length=region + 256), exp_a, "set")
    for name, (img, exp, ctrl) in tests.items():
        open(os.path.join(out, name + ".bin"), "wb").write(img)
        open(os.path.join(out, name + ".exp"), "wb").write(exp)
        open(os.path.join(out, name + ".ctrl"), "w").write(ctrl)
        print("%-10s region expect %s, control block %s" % (name, "B" if exp == exp_b else "A", ctrl))


if __name__ == "__main__":
    main(sys.argv[1:])
