"""Build the SpoolHard filaments.jsonl from BambuStudio profile JSONs.

Replaces the older filaments.db (SQLite) pipeline. The console firmware
reads JSONL natively — no SQLite client needed on-device — and the React
frontend parses the same file via fetch + JSON.parse, dropping the
~1MB sql.js WASM dependency that the SQLite path required.

The on-disk shape per row mirrors the firmware's FilamentRecord struct
(console/firmware/include/filament_record.h); see jsonl_writer.py for
the schema details.
"""

from parser import FilamentParser
from jsonl_writer import write_jsonl
import os


def main():
    out_path = "filaments.jsonl"
    if os.path.exists(out_path):
        os.remove(out_path)

    parser = FilamentParser(".")
    print("Loading all JSON files...")
    parser.load_all()
    print(f"Found {len(parser.registry)} entities. Resolving + writing JSONL...")

    written = write_jsonl(parser, out_path)
    size = os.path.getsize(out_path)
    print(f"Done! Wrote {written} resolved filament rows to {out_path} ({size} bytes)")


if __name__ == "__main__":
    main()
