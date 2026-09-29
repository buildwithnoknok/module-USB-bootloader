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

The .bin must be the OFFSET-LINKED app image (linked at 0x2000 via app.ld).

Usage:
  python3 usb_flash.py app.bin                  # the only noknok USB module attached
  python3 usb_flash.py app.bin --serial CDAB... # pick one by its USB serial (app mode)

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("--serial", help="USB serial of the module (as shown in app mode)")
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
        s.reset_input_buffer()
        s.write(b"\x01"); s.flush(); status(s, "ERASE"); print("  ERASE: OK")
        for off in range(0, len(image), CHUNK):
            part = image[off:off + CHUNK]
            s.write(bytes([0x02, len(part)]) + part); s.flush()
            status(s, "WRITE @%d" % off)
        print("  WRITE: OK (%d bytes)" % len(image))
        s.write(b"\x04" + crc.to_bytes(4, "little")); s.flush()
        status(s, "VERIFY"); print("  VERIFY: OK (app accepted)")
        s.write(b"\x05"); s.flush()
    print("BOOT sent")
    for _ in range(40):
        time.sleep(0.25)
        if ports(PID_APP, a.serial):
            print("App is back:", ports(PID_APP, a.serial)[0][0]); return
    sys.exit("app did not re-enumerate after BOOT")


if __name__ == "__main__":
    main()
