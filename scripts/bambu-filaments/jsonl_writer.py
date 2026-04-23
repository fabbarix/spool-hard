"""Flat-JSONL writer for resolved filament presets.

Replaces the SQLite database step. The on-device firmware doesn't have a
SQLite client; the React frontend used sql.js to walk inheritance in the
browser. Both go away when we ship a pre-resolved JSONL: one line per
instantiable filament with every property already merged from the
inheritance chain. ~98 rows for the BambuStudio set.

Output schema mirrors the firmware's FilamentRecord
(console/firmware/include/filament_record.h) so the same struct loads
both stock and user filaments.
"""

import json
import re
from typing import Any, Dict, List


# Keys that look like quoted strings ("\"Devil Design\"") in BambuStudio
# JSON. We strip the wrapping quotes so the firmware + frontend get the
# bare string.
_QUOTED_STRING_KEYS = {"filament_vendor", "filament_type", "filament_settings_id"}

def _unquote(v: Any) -> str:
    """`"\"Foo\""` → `Foo`. List → first element."""
    if isinstance(v, list):
        v = v[0] if v else ""
    if not isinstance(v, str):
        return str(v) if v is not None else ""
    s = v.strip()
    # BambuStudio JSON often double-quotes string values.
    if len(s) >= 2 and s.startswith('"') and s.endswith('"'):
        s = s[1:-1]
    return s


def _first_int(v: Any) -> int:
    """For comma-separated multi-value fields like `nozzle_temperature` =
    `"225,220"` we pull the first value. Returns -1 if no parseable int."""
    if isinstance(v, list):
        v = v[0] if v else None
    if v is None:
        return -1
    s = str(v).strip()
    m = re.match(r"^(\d+)", s)
    if not m:
        return -1
    try:
        return int(m.group(1))
    except (TypeError, ValueError):
        return -1


def _first_float(v: Any) -> float:
    if isinstance(v, list):
        v = v[0] if v else None
    if v is None:
        return 0.0
    try:
        return float(str(v).strip())
    except (TypeError, ValueError):
        return 0.0


def _row_from_resolved(name: str, resolved: Dict[str, Any]) -> Dict[str, Any]:
    """Build one JSONL row from a fully-resolved-via-inheritance filament dict.

    Field naming matches FilamentRecord on the firmware side; the
    frontend's FilamentEntry consumes the same shape (translated in
    useFilamentsDb).
    """
    # Bambu's nozzle_temperature is a comma-separated "<print>,<initial>"
    # pair (e.g. "225,220"). We surface them as min (initial) / max (print)
    # so the spool's nozzle_temp_min/max line up with what the slicer
    # actually sends to the printer.
    nt_print   = _first_int(resolved.get("nozzle_temperature"))
    nt_initial = _first_int(resolved.get("nozzle_temperature_initial_layer"))
    if nt_initial < 0:
        nt_initial = nt_print
    if nt_print < 0:
        nt_print = nt_initial

    # Filament_vendor in profiles is rendered as `["\"Bambu Lab\""]`. The
    # name suffix often carries the vendor too — fall back to that if the
    # field is missing. Same trick the SQLite frontend already does.
    vendor = _unquote(resolved.get("filament_vendor", ""))
    material_type = _unquote(resolved.get("filament_type", ""))

    # base_id is the stock parent id Bambu uses internally (GFSA00 etc.).
    # Stored as `setting_id` in BambuStudio's filament JSONs — we surface
    # it under the more-descriptive name `base_id` because our own
    # FilamentRecord.setting_id is the row's own id.
    base_id = _unquote(resolved.get("setting_id", ""))

    return {
        # PK — for stock rows we use the preset name verbatim. It's
        # unique per the BambuStudio set, stable across rebuilds, and
        # human-readable in serial logs / spool records that link to it.
        "setting_id":       name,
        "stock":            True,
        "name":             name,
        "base_id":          base_id,
        "filament_type":    material_type.upper() if material_type else "",
        "filament_subtype": _unquote(resolved.get("filament_subtype", "")),
        "filament_vendor":  vendor,
        "filament_id":      _unquote(resolved.get("filament_id", "")),
        "nozzle_temp_min":  nt_initial if nt_initial > 0 else -1,
        "nozzle_temp_max":  nt_print   if nt_print   > 0 else -1,
        "density":          _first_float(resolved.get("filament_density")),
        "pressure_advance": _first_float(resolved.get("pressure_advance")),
        # Stock rows are never cloud-synced. Empty strings + 0 epochs
        # match the firmware's default-constructed FilamentRecord.
        "cloud_setting_id": "",
        "cloud_synced_at":  0,
        "updated_at":       0,
    }


def write_jsonl(parser, output_path: str) -> int:
    """Walk every preset that should appear in the picker (the post-dedup
    `@base` set, mirroring database.py's `dedupe_variants` logic) and emit
    one resolved row per filament. Returns the number of rows written.

    Dedup criteria — same shape as database.py:
      - keep `@base` rows that carry a `filament_id` (these are what users
        actually select; ~98 of them after dedup)
      - drop variant rows (printer/nozzle-specific overlays — the @base
        parent already carries everything the picker needs)
    """
    rows: List[Dict[str, Any]] = []
    for name, raw in parser.registry.items():
        if not isinstance(raw, dict):
            continue
        if raw.get("type") != "filament":
            continue
        # Only @base rows survive dedup — see database.py:dedupe_variants
        # for the rationale. We'd otherwise emit ~1600 near-identical
        # variants, all with the same picker-relevant metadata.
        if not name.endswith("@base"):
            continue
        if not raw.get("filament_id"):
            # Skip non-instantiable templates (parents like `fdm_filament_pla`)
            # that lack a filament_id.
            continue
        try:
            resolved = parser.resolve_filament(name)
        except Exception as exc:
            print(f"[jsonl] resolve failed for {name}: {exc}")
            continue
        rows.append(_row_from_resolved(name, resolved))

    rows.sort(key=lambda r: r["name"])
    with open(output_path, "w") as f:
        for r in rows:
            f.write(json.dumps(r, separators=(",", ":")) + "\n")
    return len(rows)
