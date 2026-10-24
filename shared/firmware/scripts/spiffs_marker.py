#!/usr/bin/env python3
"""SPIFFS product-marker helpers (shared by both products' build paths).

The firmware's /api/upload/spiffs handler stages the first 4 KB of an
uploaded image and rejects a wrong-product marker BEFORE erasing the
partition. mkspiffs lays files out in readdir() order, which we can't
control, so the marker would land anywhere. Fix: embed the marker in EVERY
packed file's first bytes, so whichever file mkspiffs writes first carries
it near the start of the image:

  * *.gz files get the marker as the gzip FCOMMENT header field (RFC 1952;
    ignored by browsers/zlib, and sits in the first ~60 bytes of the file).
  * raw marker files (.spoolhard-product / .spoolhard-version) start with it.

CLI:
  spiffs_marker.py gzip  <product-signature> <file>...   gzip -9 in place w/ comment
  spiffs_marker.py check <product-signature> <spiffs.bin> [max_offset=3500]
      exits 1 if the signature is missing or first occurs at >= max_offset.
"""
import struct
import sys
import zlib

MARKER_PREFIX = b"SPOOLHARD-PRODUCT="
DEFAULT_MAX_OFFSET = 3500   # firmware stages 4096 B; keep margin


def gzip_with_comment(src_path, dst_path, comment: bytes):
    data = open(src_path, "rb").read()
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = co.compress(data) + co.flush()
    hdr = b"\x1f\x8b\x08\x10" + struct.pack("<I", 0) + b"\x02\xff" + comment + b"\x00"
    trailer = struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF)
    with open(dst_path, "wb") as f:
        f.write(hdr + body + trailer)


def marker_offset(image_path, signature: bytes):
    """First offset of `signature` in the image, or -1."""
    return open(image_path, "rb").read().find(signature)


def check_image(image_path, signature: str, max_offset=DEFAULT_MAX_OFFSET):
    sig = signature.encode()
    img = open(image_path, "rb").read()
    off = img.find(sig)
    other = img.find(MARKER_PREFIX)
    if off < 0:
        return False, f"product signature {signature!r} not found in {image_path}"
    if other < off and other >= 0:
        return False, f"a different product marker appears at offset {other}"
    if off >= max_offset:
        return False, (f"product signature first at offset {off} >= {max_offset}: "
                       f"the firmware's early wrong-product check (first 4 KB) would miss it")
    return True, f"product signature at offset {off} (< {max_offset}) OK"


def main(argv):
    if len(argv) >= 4 and argv[1] == "gzip":
        comment = argv[2].encode()
        import os
        for p in argv[3:]:
            gzip_with_comment(p, p + ".gz", comment)
            os.remove(p)
            print(f"   {os.path.basename(p)}.gz (marker in gzip header)")
        return 0
    if len(argv) >= 4 and argv[1] == "check":
        mx = int(argv[4]) if len(argv) > 4 else DEFAULT_MAX_OFFSET
        ok, msg = check_image(argv[3], argv[2], mx)
        print(("[spiffs_marker] " if ok else "[spiffs_marker] FAIL: ") + msg)
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
