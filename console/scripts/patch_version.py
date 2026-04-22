"""Pre-build: read the product VERSION file and inject FW_VERSION / FE_VERSION
as compile-time defines so include/config.h doesn't need to be edited for
each release. The product VERSION file lives at `<product>/VERSION` one
directory above the PlatformIO project.
"""
Import("env")
import os

version_path = os.path.normpath(os.path.join(env.get("PROJECT_DIR"), "..", "VERSION"))
if not os.path.isfile(version_path):
    print(f"[patch_version] WARNING: {version_path} not found — FW_VERSION defaults from config.h")
else:
    version = open(version_path).read().strip()
    env.Append(BUILD_FLAGS=[
        f'-DFW_VERSION=\\"{version}\\"',
        f'-DFE_VERSION=\\"{version}\\"',
    ])
    print(f"[patch_version] FW_VERSION = FE_VERSION = {version}")
