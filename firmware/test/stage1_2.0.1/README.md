# Test stage-1 2.0.1 (CH32V203, layout 4) - NOT a release

A version-bump-only build of the layout-4 stage-1, so a brain's USB stage-1 update pass
(DEV-298) has something newer than the released 2.0.0 to install. Do not copy it to
`firmware/bin/` and do not point the real registry at it.

| File | What |
|---|---|
| `noknok_usb_stage1_2.0.1.bin` | stage-1 2.0.1, 5572 B, crc32 `dc791dc1` |
| `index_v203_test.json` | same shape as `firmware/index_v203.json`, `url` points at this branch, `"test_only": true` |

**Source:** `firmware/src` at commit f5f97bb (= the 2.0.0 release candidate), built with
`make build EXTRA_CFLAGS=-DBL_VERSION_PATCH=1`. The only difference from 2.0.0 is the patch byte
(in the `0xB1` reply and in the `NKS1` header at +0x100: `4e4b5331 00040000 04000000 01000201`).

**Toolchain:** the bench Pi4 (`noknok`), riscv64-unknown-elf-gcc 14.2.0, ch32fun in
`~/dev/ch32fun`. The same toolchain rebuilds 2.0.0 byte-identical to `firmware/bin/noknok_usb_stage1.bin`,
and this 2.0.1 is byte-identical to the 2.0.1 used in the 26 Nov bench runs (Sam's 18/18 and
Bob's 22/22 of `tools/bench/dev237_bench.py`: 2.0.0 -> 2.0.1 -> 2.0.0 with the app pushed after each).

**Use:**
- From a Pi: `python3 tools/usb_flash.py firmware/test/stage1_2.0.1/noknok_usb_stage1_2.0.1.bin --stage1`
  -> back in the bootloader with `0xB1` = `(1,2,0,1,4)`; then push the app again.
- From a brain (DEV-298): a bench registry override whose `bootloader.stage1.CH32V203` entry is the
  content of `index_v203_test.json`. The module must already run the layout-4 full image
  (stage-0 + stage-1 2.0.0), see the main README.
- Back to 2.0.0: `--stage1 firmware/bin/noknok_usb_stage1.bin` -> `(1,2,0,0,4)`.
