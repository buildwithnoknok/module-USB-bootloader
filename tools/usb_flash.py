#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
usb_flash.py - flash an app .bin onto a noknok USB module over its USB bootloader,
from Linux (the Pi4 bench host). Same protocol and flow as usb_flash.ps1 (Windows):

  app running (PID 4E4E)? -> send 0xB0 ENTER_BOOTLOADER, wait for PID 4E42
  0x01 ERASE                      -> [state, err]
  0x02 WRITE n <n bytes> (n<=32)  -> [state, err]   (repeated over the image)
  0x04 VERIFY crc32(4 LE)         -> [state, err]   (bootloader writes the validity marker)
  0x05 BOOT                       -> no reply, device resets into the app
  --stage1 (bootloader >= 2.0.0, layout 4, DEV-237): the same transfer, closed with
  0x06 VERIFY_STAGE1 instead of 0x04; BOOT then resets, the frozen stage-0 installs
  the new stage-1, and the module comes back IN THE BOOTLOADER (the app region held
  the staged image) - push the app again afterwards. 0xB1 is read before and after.

The .bin must be the OFFSET-LINKED app image (linked at 0x2000 via app.ld).

Usage:
  python3 usb_flash.py app.bin                  # the only noknok USB module attached
  python3 usb_flash.py app.bin --serial CDAB... # pick one by its USB serial (app mode)
  python3 usb_flash.py noknok_usb_bootloader.bin --stage1   # stage-1 self-update

Needs pyserial. Exit code 0 = flashed, verified and booted.
"""
import argparse, glob, os, sys, time, zlib
import serial

VID = "1209"
PID_APP, PID_BL = "4e4e", "4e42"
CHUNK = 32


def ports(pid, serial_no=None):
    """tty paths of attached devices with our VID and the given PID (via sysfs)."""
    out = []
    for tty in glob.glob("/sys/class/tty/ttyACM*"):
        dev = os.path.realpath(os.path.join(tty, "device"))
        usbdev = os.path.dirname(dev)                       # interface -> device
        try:
            v = open(os.path.join(usbdev, "idVendor")).read().strip()
            p = open(os.path.join(usbdev, "idProduct")).read().strip()
            s = open(os.path.join(usbdev, "serial")).read().strip() if os.path.exists(os.path.join(usbdev, "serial")) else ""
        except OSError:
            continue
        if v == VID and p == pid and (serial_no is None or s.upper() == serial_no.upper()):
            out.append(("/dev/" + os.path.basename(tty), s))
    return out


def status(s, what):
    r = s.read(2)
    if len(r) < 2:
        sys.exit("%s: no reply from bootloader" % what)
    if r[1] != 0:
        sys.exit("%s: bootloader error %d (state %d)" % (what, r[1], r[0]))
    return r


def get_version(s):
    """0xB1 -> (proto, major, minor, patch, layout); stage-1 >= 2.0.0 only (1.x ignores it).
    Asked up to 3 times: right after the bootloader enumerates, the first read can come back
    empty (seen once on the bench, DEV-237 26 Nov 2026), and one miss must not read as "1.x"."""
    for _ in range(3):
        s.reset_input_buffer()
        s.write(b"\xB1"); s.flush()
        r = s.read(5)
        if len(r) == 5:
            return tuple(r)
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("--serial", help="USB serial of the module (as shown in app mode)")
    ap.add_argument("--stage1", action="store_true", help="the .bin is a stage-1 bootloader: close with VERIFY_STAGE1 (0x06)")
    a = ap.parse_args()
    image = open(a.bin, "rb").read()
    crc = zlib.crc32(image) & 0xFFFFFFFF
    print("Image: %s (%d bytes, crc32 0x%08X)" % (a.bin, len(image), crc))

    bl = ports(PID_BL)
    if not bl:
        apps = ports(PID_APP, a.serial)
        if len(apps) != 1:
            sys.exit("need exactly one noknok app device, found %d %s" % (len(apps), apps))
        print("App on %s (serial %s) - sending 0xB0 ENTER_BOOTLOADER" % apps[0])
        try:
            with serial.Serial(apps[0][0], 115200, timeout=0.5) as s:
                s.write(b"\xB0"); s.flush()
        except serial.SerialException:
            pass                                             # it resets under us - fine
        for _ in range(40):
            time.sleep(0.25)
            bl = ports(PID_BL)
            if bl:
                break
    if len(bl) != 1:
        sys.exit("need exactly one noknok USB bootloader (PID 4E42), found %d" % len(bl))
    print("Bootloader on %s" % bl[0][0])
    time.sleep(0.3)

    with serial.Serial(bl[0][0], 115200, timeout=3.0) as s:  # ERASE takes ~300 ms
        s.timeout = 0.5
        print("  0xB1 GET_VERSION:", get_version(s) or "no reply (bootloader 1.x)")
        s.timeout = 3.0
        s.reset_input_buffer()
        s.write(b"\x01"); s.flush(); status(s, "ERASE"); print("  ERASE: OK")
        for off in range(0, len(image), CHUNK):
            part = image[off:off + CHUNK]
            s.write(bytes([0x02, len(part)]) + part); s.flush()
            status(s, "WRITE @%d" % off)
        print("  WRITE: OK (%d bytes)" % len(image))
        if a.stage1:
            s.write(b"\x06" + crc.to_bytes(4, "little")); s.flush()
            status(s, "VERIFY_STAGE1"); print("  VERIFY_STAGE1: OK (stage-0 armed)")
        else:
            s.write(b"\x04" + crc.to_bytes(4, "little")); s.flush()
            status(s, "VERIFY"); print("  VERIFY: OK (app accepted)")
        s.write(b"\x05"); s.flush()
    print("BOOT sent")
    if a.stage1:
        bl = []
        for _ in range(60):                     # detach, stage-0 copy, re-enumerate
            time.sleep(0.25)
            bl = ports(PID_BL)
            if bl:
                break
        if len(bl) != 1:
            sys.exit("bootloader did not come back after the stage-1 install")
        time.sleep(0.3)
        with serial.Serial(bl[0][0], 115200, timeout=0.5) as s:
            print("Stage-1 now:", get_version(s), "- push the app next")
        return
    for _ in range(40):
        time.sleep(0.25)
        if ports(PID_APP, a.serial):
            print("App is back:", ports(PID_APP, a.serial)[0][0]); return
    sys.exit("app did not re-enumerate after BOOT")


if __name__ == "__main__":
    main()
