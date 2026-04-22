Import("env")
import subprocess, os, gzip, shutil

PROJECT_DIR = env.get("PROJECT_DIR")  # console/firmware/
FRONTEND_DIR = os.path.normpath(os.path.join(PROJECT_DIR, "..", "frontend"))
DATA_DIR = os.path.join(PROJECT_DIR, "data")

# Product identity. Must match PRODUCT_ID in include/config.h and the string
# baked into firmware .rodata by product_signature.h. The SPIFFS image needs
# the same tag planted as a plain file so the upload handler's substring
# matcher can validate it out of the raw partition stream.
PRODUCT_ID = "console"
PRODUCT_SIGNATURE = f"SPOOLHARD-PRODUCT={PRODUCT_ID}"
SIGNATURE_FILENAME = ".spoolhard-product"
VERSION_FILENAME   = ".spoolhard-version"

# Files that must NOT get gzipped — the upload-side matcher scans the raw
# SPIFFS bytes for these literal strings, so compressing them would hide
# the pattern.
RAW_FILES = {SIGNATURE_FILENAME, VERSION_FILENAME}

def _read_version():
    version_path = os.path.normpath(os.path.join(PROJECT_DIR, "..", "VERSION"))
    if not os.path.isfile(version_path):
        return "dev"
    return open(version_path).read().strip()

def build_frontend(source, target, env):
    if not os.path.exists(os.path.join(FRONTEND_DIR, "package.json")):
        print("[build_frontend] No frontend/package.json — skipping frontend build")
        return

    if not os.path.exists(os.path.join(FRONTEND_DIR, "node_modules")):
        print("[build_frontend] Installing npm dependencies...")
        subprocess.check_call(["npm", "install"], cwd=FRONTEND_DIR)

    print("[build_frontend] Building frontend...")
    subprocess.check_call(["npm", "run", "build"], cwd=FRONTEND_DIR)

    print("[build_frontend] Gzipping files in data/...")
    for root, dirs, files in os.walk(DATA_DIR):
        for f in files:
            if f.endswith('.gz') or f in RAW_FILES:
                continue
            path = os.path.join(root, f)
            gz_path = path + '.gz'
            with open(path, 'rb') as fin:
                with gzip.open(gz_path, 'wb', compresslevel=9) as fout:
                    shutil.copyfileobj(fin, fout)
            os.remove(path)
            print(f"  {os.path.relpath(gz_path, DATA_DIR)}")

    # Marker files — written *after* gzip so they stay raw. Upload-side
    # matchers scan the raw SPIFFS bytes for these literal patterns.
    fe_version = _read_version()

    sig_path = os.path.join(DATA_DIR, SIGNATURE_FILENAME)
    with open(sig_path, 'w') as f:
        f.write(PRODUCT_SIGNATURE)
    print(f"[build_frontend] Wrote {SIGNATURE_FILENAME}: {PRODUCT_SIGNATURE}")

    ver_path = os.path.join(DATA_DIR, VERSION_FILENAME)
    with open(ver_path, 'wb') as f:
        f.write(f"SPOOLHARD-VERSION={fe_version}".encode("utf-8") + b"\x01")
    print(f"[build_frontend] Wrote {VERSION_FILENAME}: SPOOLHARD-VERSION={fe_version}\\x01")

    print("[build_frontend] Done")

# Bind to the spiffs.bin output file, NOT the high-level `buildfs` target.
# The latter fires at the wrong point in scons' schedule — mkspiffs collects
# the data/ contents before the pre-action runs, so the freshly-gzipped
# files never make it into the image. Hooking the file target forces the
# pre-action through scons' dependency graph where mkspiffs can actually
# see the output. Both spiffs.bin and littlefs.bin are supported for
# forward-compat — only one is active at a time (board_build.filesystem).
for artifact in ("$BUILD_DIR/spiffs.bin", "$BUILD_DIR/littlefs.bin"):
    env.AddPreAction(artifact, build_frontend)
