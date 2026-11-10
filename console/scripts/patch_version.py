"""Pre-build: read the product VERSION file and inject FW_VERSION / FE_VERSION
as compile-time defines so include/config.h doesn't need to be edited for
each release. The product VERSION file lives at `<product>/VERSION` one
directory above the PlatformIO project.

Between releases VERSION carries a `-dev` suffix (e.g. `0.17.1-dev`); those
builds get the short git hash appended (`0.17.1-dev.07e0ace`, plus `-dirty`
with uncommitted changes) so a flashed device shows exactly what it runs.
The OTA check orders any suffixed version below the bare release, so the
next release still updates it. Release VERSIONs (no `-dev`) are untouched.
"""
Import("env")
import os
import subprocess


def _git_suffix(cwd):
    try:
        rev = subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"],
                                      cwd=cwd, stderr=subprocess.DEVNULL).decode().strip()
        dirty = subprocess.call(["git", "diff", "--quiet", "HEAD"],
                                cwd=cwd, stderr=subprocess.DEVNULL) != 0
    except (OSError, subprocess.CalledProcessError):
        return ""
    return f".{rev}" + ("-dirty" if dirty else "")


version_path = os.path.normpath(os.path.join(env.get("PROJECT_DIR"), "..", "VERSION"))
if not os.path.isfile(version_path):
    print(f"[patch_version] WARNING: {version_path} not found — FW_VERSION defaults from config.h")
else:
    version = open(version_path).read().strip()
    if version.endswith("-dev"):
        version += _git_suffix(os.path.dirname(version_path))
    env.Append(BUILD_FLAGS=[
        f'-DFW_VERSION=\\"{version}\\"',
        f'-DFE_VERSION=\\"{version}\\"',
    ])
    print(f"[patch_version] FW_VERSION = FE_VERSION = {version}")
