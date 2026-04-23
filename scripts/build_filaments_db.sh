#!/usr/bin/env bash
set -euo pipefail

# Build the SpoolHard filaments.jsonl from upstream BambuStudio profiles.
#
# Sparse-clones JUST `resources/profiles/BBL/filament/` from
# bambulab/BambuStudio (the multi-GB full repo would otherwise have to
# come down), feeds the JSONs to scripts/bambu-filaments/main.py, and
# writes the resulting filaments.jsonl into the output directory. The
# console firmware loads this file at runtime via the web UI's filaments
# upload — bundling it as a separate release artifact lets users grab a
# fresh copy without reflashing.
#
# Replaces the older filaments.db (SQLite) pipeline. Script name kept for
# CI compatibility; the artifact it produces is now JSONL.
#
# Usage:
#   ./scripts/build_filaments_db.sh                  # writes release/filaments.jsonl
#   ./scripts/build_filaments_db.sh path/to/out/dir  # writes <dir>/filaments.jsonl
#   ./scripts/build_filaments_db.sh --ref <branch>   # use non-master branch
#
# Requires: git, uv (https://github.com/astral-sh/uv), Python ≥ 3.10.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARSER_DIR="$REPO_ROOT/scripts/bambu-filaments"

UPSTREAM_REPO="https://github.com/bambulab/BambuStudio.git"
UPSTREAM_PATH="resources/profiles/BBL/filament"
UPSTREAM_REF="master"

OUTPUT_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref)  UPSTREAM_REF="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--ref <branch>] [output-dir]"
      exit 0 ;;
    *)
      [[ -z "$OUTPUT_DIR" ]] && OUTPUT_DIR="$1" || { echo "Unexpected arg: $1"; exit 1; }
      shift ;;
  esac
done
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT/release}"
OUTPUT_FILE="$OUTPUT_DIR/filaments.jsonl"

WORK_DIR="$(mktemp -d)"
trap "rm -rf $WORK_DIR" EXIT

echo "── Sparse-cloning $UPSTREAM_PATH @ $UPSTREAM_REF from upstream..."
# --filter=blob:none → lazy fetch, no blobs until needed.
# --sparse + sparse-checkout set → only the path we care about.
# --depth 1 → one commit, no history.
git clone --quiet --depth 1 --branch "$UPSTREAM_REF" \
          --filter=blob:none --sparse \
          "$UPSTREAM_REPO" "$WORK_DIR/bs"
git -C "$WORK_DIR/bs" sparse-checkout set "$UPSTREAM_PATH" >/dev/null

PROFILE_DIR="$WORK_DIR/bs/$UPSTREAM_PATH"
if [[ ! -d "$PROFILE_DIR" ]]; then
  echo "Upstream path $UPSTREAM_PATH not found at ref $UPSTREAM_REF" >&2
  exit 1
fi
COUNT=$(find "$PROFILE_DIR" -name '*.json' | wc -l)
COMMIT=$(git -C "$WORK_DIR/bs" rev-parse --short HEAD)
echo "   pulled $COUNT JSON profiles (BambuStudio @ $COMMIT)"

echo "── Resolving inheritance → JSONL..."
mkdir -p "$OUTPUT_DIR"
# main.py uses cwd as the input directory and writes filaments.jsonl to
# cwd. PYTHONPATH lets it import its sibling parser/jsonl_writer modules
# from the parser dir. uv handles the pydantic dependency.
( cd "$PROFILE_DIR" && \
    PYTHONPATH="$PARSER_DIR" uv --quiet --project "$PARSER_DIR" run \
        python "$PARSER_DIR/main.py" )

mv "$PROFILE_DIR/filaments.jsonl" "$OUTPUT_FILE"

SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)
SHA=$(sha256sum "$OUTPUT_FILE" | cut -d' ' -f1)
ROWS=$(wc -l < "$OUTPUT_FILE")
echo
echo "══════════════════════════════════════════════════════"
echo "  filaments.jsonl  →  $OUTPUT_FILE"
echo "  rows:    $ROWS"
echo "  size:    $SIZE"
echo "  sha256:  $SHA"
echo "  source:  bambulab/BambuStudio @ $COMMIT"
echo "══════════════════════════════════════════════════════"
