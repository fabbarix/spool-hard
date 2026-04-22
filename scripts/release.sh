#!/usr/bin/env bash
set -euo pipefail

# SpoolHard — Monorepo release builder.
# Builds the Scale and the Console in turn, places each product's
# firmware.bin / frontend.bin / manifest.json under release/<product>/.
# Versions come from each product's own VERSION file.
#
# Usage:
#   ./scripts/release.sh
#   ./scripts/release.sh --scale-base-url   https://ota.example.com/scale/0.1
#   ./scripts/release.sh --console-base-url https://ota.example.com/console/0.1
#   ./scripts/release.sh --only scale            # or --only console
#   ./scripts/release.sh --output-dir dist       # default: release/

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

OUT="$ROOT/release"
SCALE_BASE=""
CONSOLE_BASE=""
ONLY=""
WITH_FILAMENTS=0
FILAMENTS_REF="master"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale-base-url)   SCALE_BASE="$2";   shift 2 ;;
    --console-base-url) CONSOLE_BASE="$2"; shift 2 ;;
    --output-dir)       OUT="$2";          shift 2 ;;
    --only)             ONLY="$2";         shift 2 ;;
    --with-filaments)   WITH_FILAMENTS=1;  shift   ;;
    --filaments-ref)    FILAMENTS_REF="$2"; WITH_FILAMENTS=1; shift 2 ;;
    -h|--help)
      cat <<EOF
Usage: $0 [options]

  --scale-base-url URL    OTA base URL stamped into scale manifest.json
  --console-base-url URL  OTA base URL stamped into console manifest.json
  --output-dir DIR        Output root (default: release/)
  --only scale|console    Build a single product
  --with-filaments        Also build release/filaments.db from upstream BBL profiles
  --filaments-ref BRANCH  BambuStudio branch/tag to source profiles from (implies --with-filaments)
EOF
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

build_product() {
  local product=$1
  local base_url=$2
  local version
  version=$(tr -d '[:space:]' < "$ROOT/$product/VERSION")

  echo
  echo "══════════════════════════════════════════════════════"
  echo "  $product  →  $version"
  echo "══════════════════════════════════════════════════════"

  local args=(--output-dir "$OUT/$product")
  [[ -n "$base_url" ]] && args+=(--base-url "$base_url")

  "$ROOT/$product/scripts/release.sh" "${args[@]}"
}

mkdir -p "$OUT"

if [[ -z "$ONLY" || "$ONLY" == "scale" ]];   then build_product scale   "$SCALE_BASE";   fi
if [[ -z "$ONLY" || "$ONLY" == "console" ]]; then build_product console "$CONSOLE_BASE"; fi

if [[ "$WITH_FILAMENTS" == "1" ]]; then
  echo
  echo "══════════════════════════════════════════════════════"
  echo "  filaments.db  (BambuStudio @ $FILAMENTS_REF)"
  echo "══════════════════════════════════════════════════════"
  "$SCRIPT_DIR/build_filaments_db.sh" --ref "$FILAMENTS_REF" "$OUT"
fi

echo
echo "══════════════════════════════════════════════════════"
echo "  Release bundle: $OUT"
echo "══════════════════════════════════════════════════════"
find "$OUT" -maxdepth 2 -type f -printf '  %p  %s bytes\n' | sort
