#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
dev237_bench.py - DEV-237 USB-side bench run for the CH32V203 layout-4 bootloader
(frozen stage-0 + stage-1 2.0.x) on a noknok LEDs 16x plugged straight into the Pi4.

Runs on the old Pi4 `noknok`. Power cuts = GANGED uhubctl on hub 2 (cuts VBUS of all
four USB-A ports, incl. the LinkE: the board has no supply left). A cut that lands in
the bootloader is proven a real power-on by 0xB2 (RSTSCKR PORRSTF).

  BENCH_SUDO_PW=<pi sudo password> python3 dev237_bench.py <step> [...]   (uhubctl needs root)
  steps: l4ota retention stage1 cuts wrong noapp final   (or: all)
Each check prints "PASS <name> ..." or "FAIL <name> ..."; the summary is at the end.
"""
import os, subprocess, sys, time, zlib, threading
import serial

sys.path.insert(0, os.path.expanduser("~/dev/ub_237/tools"))
import usb_flash as uf                                   # ports(), PID_APP, PID_BL

B = os.path.expanduser("~/runs/2026-11-25_DEV-237_old-pi4/bins")
APP = os.path.join(B, "app.bin")
S200 = os.path.join(B, "s1_200.bin")
S201 = os.path.join(B, "s1_201.bin")
WRONG = {
    "app 2.2.2 (8488 B)": APP,
    "V003 stage-1": os.path.expanduser("~/dev/bl_dev22/firmware/bin/noknok_stage1.bin"),
    "X035 stage-1": os.path.expanduser("~/dev/bl_dev22/firmware/bin/noknok_stage1_x035.bin"),
    "USB bootloader 1.1.0": os.path.expanduser("~/dev/ub_237/firmware/bin/noknok_usb_bootloader.bin"),
}
UF = os.path.expanduser("~/dev/ub_237/tools/usb_flash.py")
PORRSTF = 1 << 27
RESULTS = []


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    log("PASS" if ok else "FAIL", name, detail)
    return ok


# ---------------- power ----------------
_root = None


def root():
    """One persistent root shell, so a cut costs only the uhubctl run, not a sudo start."""
    global _root
    if _root is None or _root.poll() is not None:
        _root = subprocess.Popen(["sudo", "-S", "-p", "", "bash"], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        # sudo password from the environment (never in the repo): BENCH_SUDO_PW=... python3 dev237_bench.py
        _root.stdin.write(os.environ.get("BENCH_SUDO_PW", "") + "\n"); _root.stdin.flush()
        rsh("true")
    return _root


def rsh(cmd):
    p = root() if cmd != "true" else _root
    p.stdin.write(cmd + " >/dev/null 2>&1; echo __done__\n"); p.stdin.flush()
    while p.stdout.readline().strip() != "__done__":
        pass


def power(on):
    rsh("/usr/sbin/uhubctl -l 2 -a %s" % ("on" if on else "off"))


def cut(off_s=3.0):
    t0 = time.monotonic(); power(False); t1 = time.monotonic()
    time.sleep(off_s); power(True)
    return t1 - t0


# ---------------- device ----------------
def wait_any(t=20.0):
    """-> ('app'|'bl', tty) or (None, None)."""
    end = time.monotonic() + t
    while time.monotonic() < end:
        b = uf.ports(uf.PID_BL)
        if b:
            time.sleep(0.4); return "bl", b[0][0]
        a = uf.ports(uf.PID_APP)
        if a:
            time.sleep(0.4); return "app", a[0][0]
        time.sleep(0.2)
    return None, None


def gone(t=3.0):
    end = time.monotonic() + t
    while time.monotonic() < end:
        if not uf.ports(uf.PID_BL) and not uf.ports(uf.PID_APP):
            return True
        time.sleep(0.05)
    return False


def ask(tty, data, n, timeout=1.0):
    with serial.Serial(tty, 115200, timeout=timeout) as s:
        s.reset_input_buffer(); s.write(data); s.flush()
        return s.read(n)


def app_alive(tty):
    r = ask(tty, b"\x30", 16)                         # LEDs 16x GET_STATUS
    return len(r) == 16, (r[0] if r else None)


def to_bl():
    kind, tty = wait_any(5)
    if kind == "bl":
        return tty
    if kind == "app":
        try:
            ask(tty, b"\xB0", 0, 0.3)
        except serial.SerialException:
            pass
        end = time.monotonic() + 10
        while time.monotonic() < end:
            b = uf.ports(uf.PID_BL)
            if b:
                time.sleep(0.4); return b[0][0]
            time.sleep(0.2)
    return None


def bl_info(tty):
    v = ask(tty, b"\xB1", 5, 0.5)
    d = ask(tty, b"\xB2", 5, 0.5)
    st = ask(tty, b"\x03", 2, 0.5)
    rst = int.from_bytes(d[1:5], "little") if len(d) == 5 else 0
    return (tuple(v) if len(v) == 5 else None), (d[0] if d else None, "%08x" % rst, bool(rst & PORRSTF)), (tuple(st) if len(st) == 2 else None)


def transfer(tty, img, close=None, cut_at=None):
    """ERASE + WRITEs (+ close 'app' 0x04 / 'stage1' 0x06). cut_at = byte offset -> power off there.
    Returns (status after close or None, cut_latency)."""
    crc = zlib.crc32(img) & 0xFFFFFFFF
    with serial.Serial(tty, 115200, timeout=3.0) as s:
        s.reset_input_buffer()
        s.write(b"\x01"); s.flush(); r = s.read(2)
        if tuple(r) != (2, 0) and (len(r) < 2 or r[1] != 0):
            return ("erase", tuple(r)), None
        for off in range(0, len(img), 32):
            if cut_at is not None and off >= cut_at:
                t0 = time.monotonic(); power(False); return ("cut@%d" % off,), time.monotonic() - t0
            part = img[off:off + 32]
            s.write(bytes([0x02, len(part)]) + part); s.flush()
            r = s.read(2)
            if len(r) < 2 or r[1] != 0:
                return ("write@%d" % off, tuple(r)), None
        if close is None:
            return ("written",), None
        s.write((b"\x04" if close == "app" else b"\x06") + crc.to_bytes(4, "little")); s.flush()
        return tuple(s.read(2)), None


def uflash(path, stage1=False):
    cmd = ["python3", UF, path] + (["--stage1"] if stage1 else [])
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    out = (p.stdout + p.stderr).strip()
    for line in out.splitlines():
        log("   |", line)
    return p.returncode == 0, out


def push_app(name):
    ok, _ = uflash(APP)
    kind, tty = wait_any(10)
    alive = app_alive(tty)[0] if kind == "app" else False
    return check(name, ok and alive, "usb_flash rc ok=%s, app answers 0x30=%s" % (ok, alive))


def stage1_install(path, want, name):
    ok, out = uflash(path, stage1=True)
    tty = to_bl()
    v = bl_info(tty)[0] if tty else None
    return check(name, ok and v == want, "0xB1=%s want %s" % (v, want))


# ---------------- steps ----------------
def step_l4ota():
    kind, tty = wait_any(10)
    check("L4 boots the app after SWD", kind == "app" and app_alive(tty)[0], "kind=%s" % kind)
    tty = to_bl()
    v, d, st = bl_info(tty) if tty else (None, None, None)
    check("0xB0 -> bootloader, 0xB1 = (1,2,0,0,4)", v == (1, 2, 0, 0, 4), "0xB1=%s diag=%s status=%s" % (v, d, st))
    push_app("app OTA through stage-1 2.0.0")


def step_retention(n=10):
    fails = 0
    for i in range(1, n + 1):
        lat = cut(3.0)
        kind, tty = wait_any(20)
        alive = app_alive(tty)[0] if kind == "app" else False
        log("   cycle %d: %s alive=%s (off latency %.2fs)" % (i, kind, alive, lat))
        fails += 0 if alive else 1
    check("app retention %dx power cut (OTA-written app)" % n, fails == 0, "%d/%d back as the app" % (n - fails, n))


def step_stage1():
    stage1_install(S201, (1, 2, 0, 1, 4), "stage-1 self-update 2.0.0 -> 2.0.1")
    push_app("app push after stage-1 2.0.1")
    stage1_install(S200, (1, 2, 0, 0, 4), "stage-1 back 2.0.1 -> 2.0.0")
    push_app("app push after stage-1 2.0.0")


def after_cut(name, want, off_s=3.0):
    time.sleep(off_s); power(True)
    kind, tty = wait_any(20)
    if kind != "bl":
        return check(name, False, "came back as %s" % kind)
    v, d, st = bl_info(tty)
    return check(name, v == want and d[2], "0xB1=%s want %s, diag branch/rst/POR=%s, status=%s" % (v, want, d, st))


def step_cuts():
    cur = (1, 2, 0, 0, 4)
    imgs = {(1, 2, 0, 0, 4): open(S200, "rb").read(), (1, 2, 0, 1, 4): open(S201, "rb").read()}
    other = lambda v: (1, 2, 0, 1, 4) if v == (1, 2, 0, 0, 4) else (1, 2, 0, 0, 4)

    # (a) cut mid-WRITE: nothing armed -> old stage-1, flash mode
    tty = to_bl()
    r, lat = transfer(tty, imgs[other(cur)], close="stage1", cut_at=len(imgs[other(cur)]) // 2)
    log("   (a) %s latency %.2fs" % (r, lat or 0))
    after_cut("cut (a) mid-WRITE -> old stage-1 in flash mode", cur)

    # (b) cut after VERIFY_STAGE1, before BOOT: armed -> stage-0 installs at power-on
    tty = to_bl(); new = other(cur)
    r, _ = transfer(tty, imgs[new], close="stage1")
    log("   (b) VERIFY_STAGE1 -> %s" % (r,))
    power(False)
    if after_cut("cut (b) after VERIFY_STAGE1, before BOOT -> new stage-1", new):
        cur = new

    # (c) BOOT, then cut after d seconds (stage-0 copy window)
    for d in (0.0, 0.05, 0.2):
        tty = to_bl(); new = other(cur)
        r, _ = transfer(tty, imgs[new], close="stage1")
        with serial.Serial(tty, 115200, timeout=0.5) as s:
            s.write(b"\x05"); s.flush(); tb = time.monotonic()
        time.sleep(d); power(False); lat = time.monotonic() - tb
        log("   (c) VERIFY_STAGE1 %s, BOOT -> power off after %.3fs" % (r, lat))
        if after_cut("cut (c) %.2fs after BOOT (real %.2fs) -> new stage-1" % (d, lat), new):
            cur = new

    # (d) cut with the bootloader back: same stage-1, flash mode
    tty = to_bl(); power(False)
    after_cut("cut (d) bootloader idle -> same stage-1", cur)

    # end on 2.0.0 with the app
    if cur != (1, 2, 0, 0, 4):
        stage1_install(S200, (1, 2, 0, 0, 4), "cuts: back to stage-1 2.0.0")
    push_app("app push after the power cuts")


def step_wrong():
    for name, path in WRONG.items():
        img = open(path, "rb").read()
        tty = to_bl()
        v0 = bl_info(tty)[0]
        r, _ = transfer(tty, img, close="stage1")
        st = ask(tty, b"\x03", 2, 0.5)
        with serial.Serial(tty, 115200, timeout=0.5) as s:
            s.write(b"\x05"); s.flush()
        left = gone(2.5)                                  # an armed update would reset (detach)
        st2 = ask(tty, b"\x03", 2, 0.5) if not left else b""
        v1 = bl_info(tty)[0] if not left else None
        ok = r[0] == 3 and r[1] in (4, 8) and not left and tuple(st2) == (3, 6) and v1 == v0
        check("wrong file --stage1: %s (%d B)" % (name, len(img)), ok,
              "VERIFY_STAGE1=%s READ_STATUS=%s, BOOT: detached=%s status=%s, 0xB1 %s->%s"
              % (r, tuple(st), left, tuple(st2), v0, v1))
        if left:
            wait_any(20)


def step_noapp():
    tty = to_bl()
    with serial.Serial(tty, 115200, timeout=3.0) as s:
        s.reset_input_buffer(); s.write(b"\x01"); s.flush(); e = s.read(2)
        s.write(b"\x05"); s.flush()
    left = gone(2.5)
    st = ask(tty, b"\x03", 2, 0.5) if not left else b""
    check("BOOT without a valid app stays in the bootloader, error 6",
          tuple(e)[1:] == (0,) and not left and tuple(st) == (3, 6),
          "ERASE=%s detached=%s READ_STATUS=%s" % (tuple(e), left, tuple(st)))
    push_app("app push after the no-app test")


def step_final():
    kind, tty = wait_any(10)
    ok, ver = app_alive(tty) if kind == "app" else (False, None)
    check("final: app running", ok, "kind=%s fw byte=%s" % (kind, ver))


STEPS = dict(l4ota=step_l4ota, retention=step_retention, stage1=step_stage1,
             cuts=step_cuts, wrong=step_wrong, noapp=step_noapp, final=step_final)

if __name__ == "__main__":
    todo = sys.argv[1:] or ["all"]
    if todo == ["all"]:
        todo = ["l4ota", "retention", "stage1", "cuts", "wrong", "noapp", "final"]
    log("=== dev237_bench", todo)
    try:
        for st in todo:
            log("--- step", st)
            STEPS[st]()
    finally:
        try:
            power(True)
        except Exception:
            pass
        log("=== SUMMARY: %d PASS, %d FAIL" % (sum(r[1] for r in RESULTS), sum(not r[1] for r in RESULTS)))
        for n, ok, d in RESULTS:
            print("  %s  %s  %s" % ("PASS" if ok else "FAIL", n, d))
