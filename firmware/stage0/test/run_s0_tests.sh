#!/bin/sh
# SPDX-License-Identifier: MIT
# run_s0_tests.sh <dir from mk_s0_tests.py> - flash each stage-0 test image to the CH32V203 on
# the WCH-LinkE, let stage-0 run, read the stage-1 region and the control block back over SWD,
# compare with the expectation. PASS/FAIL per image. DEV-237.
D=$1
M=${M:-/home/noknok/dev/ch32fun/minichlink/minichlink}
for img in "$D"/t_*.bin; do
  n=$(basename "$img" .bin)
  $M -w "$img" flash -b >/dev/null 2>&1
  sleep 3                                   # stage-0: 50 ms settle + copy + reset, then stage-1/app
  $M -r "$D/$n.read" 0x08000400 7168 >/dev/null 2>&1
  $M -r "$D/$n.ctrl.read" 0x08007E00 4 >/dev/null 2>&1
  $M -b >/dev/null 2>&1
  c=$(od -A n -t x4 "$D/$n.ctrl.read" 2>/dev/null | tr -d ' ')
  want=$(cat "$D/$n.ctrl")
  if [ "$want" = "blank" ]; then okc=$([ "$c" = "ffffffff" ] && echo y); else okc=$([ "$c" = "6e6b5530" ] && echo y); fi
  if cmp -s "$D/$n.read" "$D/$n.exp" && [ "$okc" = "y" ]; then r=PASS; else r=FAIL; fi
  echo "$r  $n  (stage-1 region $(cmp -s "$D/$n.read" "$D/$n.exp" && echo as-expected || echo DIFFERENT), control word $c, want $want)"
done
