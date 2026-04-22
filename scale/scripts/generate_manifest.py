#!/usr/bin/env python3
"""Generate an OTA manifest.json from built firmware and SPIFFS images."""

import argparse, hashlib, json, os, re, sys

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()

def read_version(config_h, define_name):
    pat = re.compile(rf'#define\s+{define_name}\s+"([^"]+)"')
    with open(config_h) as f:
        for line in f:
            m = pat.match(line.strip())
            if m:
                return m.group(1)
    return "unknown"

def read_fe_version(package_json):
    with open(package_json) as f:
        pkg = json.load(f)
    return pkg.get("version", "unknown")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--firmware", required=True, help="Path to firmware.bin")
    p.add_argument("--spiffs", required=True, help="Path to spiffs.bin")
    p.add_argument("--config", default="include/config.h")
    p.add_argument("--package", default="frontend/package.json")
    p.add_argument("--base-url", required=True, help="Base URL for download")
    p.add_argument("--output", default="manifest.json")
    args = p.parse_args()

    fw_version = read_version(args.config, "FW_VERSION")
    fe_version = read_fe_version(args.package) if os.path.exists(args.package) else read_version(args.config, "FE_VERSION")

    manifest = {
        "firmware": {
            "version": fw_version,
            "url": f"{args.base_url}/firmware.bin",
            "size": os.path.getsize(args.firmware),
            "sha256": sha256_file(args.firmware),
        },
        "frontend": {
            "version": fe_version,
            "url": f"{args.base_url}/frontend.bin",
            "size": os.path.getsize(args.spiffs),
            "sha256": sha256_file(args.spiffs),
        },
    }

    with open(args.output, 'w') as f:
        json.dump(manifest, f, indent=2)
    print(f"Manifest written to {args.output}")
    print(json.dumps(manifest, indent=2))

if __name__ == "__main__":
    main()
