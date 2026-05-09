"""Flat-JSONL writer for resolved filament presets.

The on-device firmware doesn't have a SQLite client; the React frontend
used sql.js to walk inheritance in the browser. Both go away when we
ship a pre-resolved JSONL: one line per logical filament with every
identity field merged from the inheritance chain, and an embedded
`variants` array carrying per-(printer, nozzle) settings drawn from
each instantiable child preset.

Output schema mirrors the firmware's FilamentRecord
(console/firmware/include/filament_record.h) so the same struct loads
both stock and user filaments.
"""

import json
import re
from typing import Any, Dict, List, Optional, Tuple


# Keys whose values come back wrapped in extra quotes (`"\"PLA\""`); we
# strip the wrapping for both firmware + frontend.
_QUOTED_STRING_KEYS = {"filament_vendor", "filament_type", "filament_settings_id"}


def _unquote(v: Any) -> str:
    if isinstance(v, list):
        v = v[0] if v else ""
    if not isinstance(v, str):
        return str(v) if v is not None else ""
    s = v.strip()
    if len(s) >= 2 and s.startswith('"') and s.endswith('"'):
        s = s[1:-1]
    return s


def _first_int(v: Any) -> int:
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


def _nth_float(v: Any, idx: int) -> float:
    if isinstance(v, list) and idx < len(v):
        try:
            return float(str(v[idx]).strip())
        except (TypeError, ValueError):
            return 0.0
    return 0.0


# Bambu's compatible_printers entries look like
#   "Bambu Lab H2S 0.4 nozzle"
#   "Bambu Lab X1 Carbon 0.6 nozzle"
#   "Bambu Lab A1 mini 0.4 nozzle"
# We canonicalise model names to the same short codes the firmware uses
# in modelFromSerial (X1C, X1, P1S, P1P, A1, A1mini, H2D, H2S).
def _parse_compatible(s: str) -> Optional[Tuple[str, float]]:
    if " nozzle" not in s:
        return None
    n_pos = s.index(" nozzle")
    space_before = s.rfind(" ", 0, n_pos)
    if space_before < 0:
        return None
    try:
        diameter = float(s[space_before + 1:n_pos])
    except ValueError:
        return None
    model = s[:space_before].strip()
    if model.startswith("Bambu Lab "):
        model = model[len("Bambu Lab "):].strip()
    if model == "X1 Carbon":
        model = "X1C"
    elif model == "A1 mini":
        model = "A1mini"
    else:
        model = model.replace(" ", "")
    return model, diameter


def _group_key(name: str) -> str:
    """Logical filament key — the part of the preset name before " @"."""
    at = name.find(" @")
    return name[:at] if at >= 0 else name


def _identity_from_resolved(name: str, resolved: Dict[str, Any]) -> Dict[str, Any]:
    """Pull the identity + range fields shared by every variant of one
    logical filament. Identity is what the user filters on in the
    picker; numeric per-(printer,nozzle) fields go on each variant."""
    nt_lo = _first_int(resolved.get("nozzle_temperature_range_low"))
    nt_hi = _first_int(resolved.get("nozzle_temperature_range_high"))
    if nt_hi < 0:
        nt_hi = _first_int(resolved.get("nozzle_temperature"))
    if nt_lo < 0:
        nt_lo = _first_int(resolved.get("nozzle_temperature_initial_layer"))
    if nt_lo < 0:
        nt_lo = nt_hi
    if nt_hi < 0:
        nt_hi = nt_lo

    vendor = _unquote(resolved.get("filament_vendor", ""))
    material_type = _unquote(resolved.get("filament_type", ""))
    base_id = _unquote(resolved.get("setting_id", ""))

    return {
        "setting_id":       _group_key(name),
        "stock":            True,
        "name":             _group_key(name),
        "base_id":          base_id,
        "filament_type":    material_type.upper() if material_type else "",
        "filament_subtype": _unquote(resolved.get("filament_subtype", "")),
        "filament_vendor":  vendor,
        "filament_id":      _unquote(resolved.get("filament_id", "")),
        "nozzle_temp_min":  nt_lo if nt_lo > 0 else -1,
        "nozzle_temp_max":  nt_hi if nt_hi > 0 else -1,
        "density":          _first_float(resolved.get("filament_density")),
        "variants":         [],
        "cloud_setting_id": "",
        "cloud_synced_at":  0,
        "updated_at":       0,
    }


def _split_extr_variants(raw: Any) -> List[str]:
    """`filament_extruder_variant` ships as a `;`-separated string,
    sometimes with each label individually quoted
    (`"Direct Drive Standard";"Direct Drive High Flow"`). Normalise to
    a clean list of labels."""
    if isinstance(raw, list):
        raw = raw[0] if raw else ""
    if not isinstance(raw, str) or not raw:
        return []
    out = []
    for tok in raw.split(";"):
        t = tok.strip()
        if t.startswith('"') and t.endswith('"'):
            t = t[1:-1].strip()
        if t:
            out.append(t)
    return out


def _all_floats(raw: Any) -> List[float]:
    if not isinstance(raw, list):
        return []
    out = []
    for v in raw:
        try:
            out.append(float(str(v).strip()))
        except (TypeError, ValueError):
            out.append(0.0)
    return out


def _variant_from_resolved(name: str, resolved: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Emit one variant entry per (printer, nozzle) the preset declares
    in `compatible_printers`. Per-extruder values
    (`filament_max_volumetric_speed`, `pressure_advance`) are kept as
    parallel arrays alongside `extruder_variants` so the same preset
    can carry distinct numbers for "Direct Drive Standard" vs "Direct
    Drive High Flow"."""
    extr_variants = _split_extr_variants(resolved.get("filament_extruder_variant", ""))

    nt_print   = _first_int(resolved.get("nozzle_temperature"))
    nt_initial = _first_int(resolved.get("nozzle_temperature_initial_layer"))
    speeds     = _all_floats(resolved.get("filament_max_volumetric_speed"))
    ks         = _all_floats(resolved.get("pressure_advance"))

    def _shared(v: Dict[str, Any]):
        if extr_variants:  v["extruder_variants"]     = extr_variants
        if nt_print > 0:   v["nozzle_temp_print"]     = nt_print
        if nt_initial > 0: v["nozzle_temp_initial_layer"] = nt_initial
        if speeds:         v["max_volumetric_speed"]  = speeds
        if ks:             v["pressure_advance"]      = ks

    out: List[Dict[str, Any]] = []
    cp = resolved.get("compatible_printers")
    if isinstance(cp, list) and cp:
        for entry in cp:
            if not isinstance(entry, str):
                continue
            parsed = _parse_compatible(entry)
            v: Dict[str, Any] = {}
            if parsed:
                v["printer_model"]    = parsed[0]
                v["nozzle_diameter"]  = parsed[1]
            else:
                v["printer_model"]    = entry
                v["nozzle_diameter"]  = 0.0
            _shared(v)
            out.append(v)
    else:
        at = name.find(" @")
        v: Dict[str, Any] = {"printer_model": "", "nozzle_diameter": 0.0}
        if at >= 0:
            parsed = _parse_compatible(name[at + 2:])
            if parsed:
                v["printer_model"]   = parsed[0]
                v["nozzle_diameter"] = parsed[1]
        _shared(v)
        out.append(v)
    return out


def write_jsonl(parser, output_path: str) -> int:
    """Walk every instantiable filament preset (anything with a
    `filament_id`) and emit one logical-filament row per group_key.
    Identity comes from the @base preset where one exists; per-printer
    variants come from the printer/nozzle-suffixed siblings.

    Dedup behaviour:
      - One row per group_key (preset name with " @<printer>..." stripped).
      - Variants merged from all siblings, deduped by (printer_model,
        nozzle_diameter).
    """
    groups: Dict[str, Dict[str, Any]] = {}
    seen_variant_keys: Dict[str, set] = {}

    for name, raw in parser.registry.items():
        if not isinstance(raw, dict):
            continue
        if raw.get("type") != "filament":
            continue
        if not raw.get("filament_id"):
            # Non-instantiable parent (e.g. fdm_filament_pla) — skip.
            continue

        try:
            resolved = parser.resolve_filament(name)
        except Exception as exc:
            print(f"[jsonl] resolve failed for {name}: {exc}")
            continue

        key = _group_key(name)
        if key not in groups:
            # First sibling we see in the group sets identity. Prefer
            # the @base preset if it shows up; otherwise the first one
            # parser emits is fine — vendor/material/density don't
            # vary across siblings.
            groups[key] = _identity_from_resolved(name, resolved)
            seen_variant_keys[key] = set()

        # If we just discovered an @base preset for an existing group,
        # promote its identity (in case the first sibling we saw was a
        # printer-specific one with sparser metadata).
        if name.endswith("@base"):
            id_ = _identity_from_resolved(name, resolved)
            for k in ("base_id", "filament_id", "filament_vendor",
                      "filament_type", "filament_subtype",
                      "nozzle_temp_min", "nozzle_temp_max", "density"):
                v = id_.get(k)
                if v not in (None, "", 0, -1):
                    groups[key][k] = v

        for v in _variant_from_resolved(name, resolved):
            vkey = (v.get("printer_model", ""), v.get("nozzle_diameter", 0.0))
            if vkey in seen_variant_keys[key]:
                continue
            seen_variant_keys[key].add(vkey)
            groups[key]["variants"].append(v)

    rows = list(groups.values())
    rows.sort(key=lambda r: r["name"])
    with open(output_path, "w") as f:
        for r in rows:
            f.write(json.dumps(r, separators=(",", ":")) + "\n")
    return len(rows)
