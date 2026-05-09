#!/usr/bin/env python3
"""Read the PCAP-binary stream emitted by the spoolhard sniffer firmware
on /dev/ttyACM0 and write it to a .pcap file. Anchors on the pcap magic
(0xa1b2c3d4) so the host can be started before or after the sniffer.

Usage:
    python3 host_capture.py [--port /dev/ttyACM0] [--out path.pcap]
                            [--seconds 180] [--restart-scale]
                            [--restart-console]

If --restart-scale or --restart-console is passed, the script POSTs
/api/restart to that device after the magic is seen, so we capture a
fresh EAPOL 4-way handshake (required for WPA2 decryption in Wireshark).
"""
import argparse
import os
import sys
import time
import threading
import urllib.request
import serial


PCAP_MAGIC = 0xa1b2c3d4


def kick_restart(target_url, header_auth, label, delay_s):
    time.sleep(delay_s)
    print(f"[host] POST {target_url}", flush=True)
    req = urllib.request.Request(target_url, method="POST",
                                  headers={"Authorization": header_auth})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            print(f"[host] {label} restart -> HTTP {r.status}", flush=True)
    except Exception as e:
        print(f"[host] {label} restart failed: {e}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--out",  default=None)
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--restart-scale", action="store_true")
    ap.add_argument("--restart-console", action="store_true")
    ap.add_argument("--scale-url",   default="http://192.168.20.154/api/restart")
    ap.add_argument("--console-url", default="http://192.168.20.153/api/restart")
    ap.add_argument("--scale-key",   default=os.environ.get("SCALINATA_KEY",""))
    ap.add_argument("--console-key", default=os.environ.get("SPULETTO_KEY",""))
    args = ap.parse_args()

    out = args.out or f"/opt/mrwho/Projects/spoolease-fw/tools/sniffer/captures/cap_{int(time.time())}.pcap"
    os.makedirs(os.path.dirname(out), exist_ok=True)

    s = serial.Serial(args.port, args.baud, timeout=1)
    print(f"[host] opened {args.port}, writing -> {out}", flush=True)

    # Send GO byte to wake the writer task. Repeat for ~3s in case the
    # firmware was mid-bringup when we connected.
    deadline = time.time() + 3
    while time.time() < deadline:
        s.write(b"G")
        s.flush()
        time.sleep(0.1)

    # Read the 24-byte pcap global header. Anchor on magic to discard any
    # pre-handshake garbage.
    hunt = bytearray()
    deadline = time.time() + 10
    while time.time() < deadline:
        chunk = s.read(4096)
        if not chunk: continue
        hunt += chunk
        idx = -1
        for i in range(len(hunt) - 3):
            if hunt[i:i+4] == bytes([0xd4, 0xc3, 0xb2, 0xa1]):
                idx = i
                break
        if idx >= 0:
            print(f"[host] pcap magic at byte {idx} (discarded {idx} pre-bytes)", flush=True)
            tail = hunt[idx:]
            with open(out, "wb") as f:
                f.write(tail)
            break
    else:
        print("[host] never saw pcap magic — is the sniffer booted? press RST and rerun.", flush=True)
        return 1

    # Trigger device restarts so we get fresh EAPOL handshakes.
    if args.restart_scale and args.scale_key:
        threading.Thread(
            target=kick_restart,
            args=(args.scale_url, f"Bearer {args.scale_key}", "scale", 1.5),
            daemon=True,
        ).start()
    if args.restart_console and args.console_key:
        threading.Thread(
            target=kick_restart,
            args=(args.console_url, f"Bearer {args.console_key}", "console", 2.0),
            daemon=True,
        ).start()

    # Stream rest of bytes for the configured window.
    end = time.time() + args.seconds
    bytes_total = 0
    last_print = time.time()
    with open(out, "ab") as f:
        while time.time() < end:
            chunk = s.read(8192)
            if chunk:
                f.write(chunk)
                bytes_total += len(chunk)
            now = time.time()
            if now - last_print > 5:
                rem = int(end - now)
                print(f"[host] {bytes_total} bytes captured, {rem}s remaining",
                      flush=True)
                last_print = now

    print(f"[host] done -> {out} ({bytes_total} bytes)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
