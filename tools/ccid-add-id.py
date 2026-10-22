#!/usr/bin/env python3
"""Add a USB VID/PID to libccid's Info.plist so pcscd on Linux uses the device.

libccid only drives readers listed in its Info.plist. macOS and Windows have
class drivers and need nothing. Run as root, then restart pcscd:

    sudo tools/ccid-add-id.py                  # 0x1209:0x0001 "fore PIV"
    sudo tools/ccid-add-id.py --vid 0x1234 --pid 0x5678
    sudo systemctl restart pcscd

Package upgrades of libccid overwrite the file, so re-run after one.
"""
import argparse
import re
import sys

DEFAULT_PLIST = "/usr/lib/pcsc/drivers/ifd-ccid.bundle/Contents/Info.plist"


def array_items(text, key):
    m = re.search(r"<key>%s</key>\s*<array>(.*?)</array>" % key, text, re.S)
    if not m:
        sys.exit("%s not found in plist" % key)
    return re.findall(r"<string>(.*?)</string>", m.group(1))


def prepend(text, key, value):
    pat = re.compile(r"(<key>%s</key>\s*<array>[ \t]*\n)" % key)
    if not pat.search(text):
        sys.exit("cannot find the %s array" % key)
    return pat.sub(lambda m: m.group(1) + "\t\t<string>%s</string>\n" % value, text, count=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vid", default="0x1209")
    ap.add_argument("--pid", default="0x0001")
    ap.add_argument("--name", default="fore PIV")
    ap.add_argument("--plist", default=DEFAULT_PLIST)
    args = ap.parse_args()

    vid = "0x%04X" % int(args.vid, 16)
    pid = "0x%04X" % int(args.pid, 16)
    with open(args.plist) as f:
        text = f.read()

    vids = [v.upper() for v in array_items(text, "ifdVendorID")]
    pids = [p.upper() for p in array_items(text, "ifdProductID")]
    if any(v == vid.upper() and p == pid.upper() for v, p in zip(vids, pids)):
        print("%s:%s already listed in %s" % (vid, pid, args.plist))
        return

    # Insert at the head of all three parallel arrays so indices stay aligned.
    text = prepend(text, "ifdVendorID", vid)
    text = prepend(text, "ifdProductID", pid)
    text = prepend(text, "ifdFriendlyName", args.name)
    with open(args.plist, "w") as f:
        f.write(text)
    print("added %s:%s (%s) to %s; restart pcscd" % (vid, pid, args.name, args.plist))


if __name__ == "__main__":
    main()
