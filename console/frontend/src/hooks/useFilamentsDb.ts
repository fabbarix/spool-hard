import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import initSqlJs, { type Database } from 'sql.js';
// Vite: emit the wasm as a hashed asset and resolve its URL at build time.
import sqlWasmUrl from 'sql.js/dist/sql-wasm.wasm?url';
import { uploadWithAuth } from '@spoolhard/ui/utils/uploadWithAuth';
import { useState } from 'react';

// A single user-facing filament preset from the bambu-filaments SQLite DB,
// flattened into the fields SpoolDetailPanel already edits.
export interface FilamentEntry {
  name: string;            // raw preset name, e.g. "Bambu PLA Basic @BBL X1C"
  filament_id: string;     // "GFL99" — what Bambu calls tray_info_idx
  brand: string;
  material: string;
  subtype: string;
  color_code?: string;     // "RRGGBB" (6 hex), if the preset pins a color
  color_name?: string;
  nozzle_temp_min?: number;
  nozzle_temp_max?: number;
  density?: number;        // g/cm³
  advertised?: number;     // grams
}

// Module-scoped sql.js handle — we only want to bootstrap wasm once per tab.
let s_SQL: Awaited<ReturnType<typeof initSqlJs>> | null = null;
async function loadSqlJs() {
  if (s_SQL) return s_SQL;
  s_SQL = await initSqlJs({ locateFile: () => sqlWasmUrl });
  return s_SQL;
}

// Parse the preset name to get brand / material / subtype cheaply. The
// bambu-filaments DB stores this info as resolved properties that would
// cost multiple SQL round-trips per row; pulling it from the name is
// good enough for search/filter and avoids doing ~1000 recursive resolves
// just to populate the picker list.
//
// Naming pattern observed across all Bambu JSON presets:
//   "<Vendor> <Material> <Subtype?> @<Profile> <Nozzle?>"
// e.g. "Bambu PLA Basic @BBL X1C 0.4 nozzle"
//      "Generic PETG HF @BBL"
//      "Bambu PA-CF @BBL P1P"
function parseFilamentName(name: string): Pick<FilamentEntry, 'brand' | 'material' | 'subtype'> {
  const head = name.split('@')[0].trim();                // drop "@profile tail"
  const parts = head.split(/\s+/);
  const brand = parts[0] ?? '';
  const material = parts[1] ?? '';
  const subtype = parts.slice(2).join(' ').trim();
  return { brand, material, subtype };
}

// Resolved shape: properties from filament_properties (child-overrides-parent)
// plus the `filament_id` column which lives on the filaments table directly
// and is always NULL on the user-facing instantiable preset — it's only set
// on `@base` ancestors, so we walk the chain to pick up the first non-null.
interface ResolvedFilament {
  props: Record<string, string[]>;
  filament_id: string;
}

// Recursively resolve a single filament's state by walking its
// `inherits_name` chain plus `filament_includes` union. Child values
// override parent values; mirrors the logic in
// /opt/mrwho/Projects/bambu-filaments/filament_client.py:_resolve_recursive.
function resolveFilament(db: Database, name: string): ResolvedFilament {
  const props: Record<string, string[]> = {};
  let filament_id = '';
  const visited = new Set<string>();

  const walk = (n: string) => {
    if (visited.has(n)) return;
    visited.add(n);

    const row = db.exec(
      'SELECT id, inherits_name, filament_id FROM filaments WHERE name = ?',
      [n],
    )[0];
    if (!row || row.values.length === 0) return;
    const fid = row.values[0][0] as number;
    const parent = row.values[0][1] as string | null;
    const fid_col = row.values[0][2] as string | null;

    // Parent first so the child can override its properties. `filament_id`
    // uses child-first priority though — the more specific ancestor wins
    // because we stop on the first non-empty hit on the way back up.
    if (parent) walk(parent);

    const inc = db.exec(
      'SELECT include_name FROM filament_includes WHERE filament_id = ? ORDER BY sequence',
      [fid],
    )[0];
    inc?.values.forEach((r) => walk(r[0] as string));

    // This filament's direct properties override whatever the ancestors set.
    const pr = db.exec(
      `SELECT k.key, v.value, fp.value_index
         FROM filament_properties fp
         JOIN property_keys   k ON fp.key_id   = k.id
         JOIN property_values v ON fp.value_id = v.id
        WHERE fp.filament_id = ?
        ORDER BY k.key, fp.value_index`,
      [fid],
    )[0];
    pr?.values.forEach(([key, value, idx]) => {
      const k = key as string;
      const v = value as string;
      const i = idx as number;
      if (!props[k]) props[k] = [];
      while (props[k].length <= i) props[k].push('');
      props[k][i] = v;
    });

    // First non-empty filament_id seen wins (child-first priority once the
    // walk unwinds — child is visited last after its parents).
    if (!filament_id && fid_col) filament_id = fid_col;
  };

  walk(name);
  return { props, filament_id };
}

// Pull the fields SpoolRecord uses from a resolved filament. Missing values
// stay undefined so SpoolDetailPanel's overlay save only touches what the
// library actually knew. This DB doesn't store colour or gross weight as
// properties (verified empirically — `filament_colour`,
// `default_filament_colour` and `filament_weight` are never inserted), so
// we skip them and leave those spool fields alone.
function entryFromResolved(name: string, r: ResolvedFilament): FilamentEntry {
  const { brand, material, subtype } = parseFilamentName(name);
  // Display name strips the "@<profile> <nozzle>" tail. The suffix just
  // disambiguates between printer/nozzle variants of the same filament —
  // meaningless after dedup by filament_id, so we drop it for cleaner rows.
  const display_name = name.split('@')[0].trim();

  const firstNum = (key: string): number | undefined => {
    const v = r.props[key]?.[0];
    if (v === undefined || v === '') return undefined;
    const n = Number(v);
    return Number.isFinite(n) ? Math.round(n) : undefined;
  };

  // Bambu's slicer presets expose TWO kinds of temperature:
  //   * nozzle_temperature              — a single target (e.g. "220")
  //   * nozzle_temperature_range_low/high — the acceptable envelope
  //     (e.g. 190–240 for PLA Basic). Only a handful of presets set these
  //     directly; the rest inherit from fdm_filament_<material>.
  // Prefer the range when present; fall back to the target temp for both
  // min and max so the user still gets *something* reasonable.
  const nozzle_temp_min = firstNum('nozzle_temperature_range_low')
    ?? firstNum('nozzle_temperature');
  const nozzle_temp_max = firstNum('nozzle_temperature_range_high')
    ?? firstNum('nozzle_temperature');

  // Filament density (g/cm³) — resolved via inheritance. Present on ~93%
  // of canonical entries in the upstream DB.
  const raw_density = r.props['filament_density']?.[0];
  const density = raw_density ? (() => {
    const n = Number(raw_density);
    return Number.isFinite(n) && n > 0 ? n : undefined;
  })() : undefined;

  // The resolved `filament_type` and `filament_vendor` are more reliable
  // than the leading name tokens (e.g. "Generic PETG HF @BBL" would parse
  // as brand=Generic, but vendor resolves to the actual origin vendor).
  const resolved_material = r.props['filament_type']?.[0] || material;
  const resolved_vendor   = r.props['filament_vendor']?.[0] || brand;

  return {
    name: display_name,
    filament_id: r.filament_id,
    brand: resolved_vendor,
    material: resolved_material,
    subtype,
    nozzle_temp_min,
    nozzle_temp_max,
    density,
  };
}

// The DB ships each filament across 30+ printer/nozzle variants (all the
// "@BBL X1C 0.4 nozzle" combos), but everything the picker cares about —
// material, subtype, filament_id, temps — is identical across variants.
// Reducing ~1600 presets to ~100 unique filaments makes the picker fast,
// the dropdown scannable, and the payload smaller.
//
// Prefer a variant with both nozzle_temp_min AND nozzle_temp_max set; some
// variants override just one of them and we'd rather keep the fullest
// record per id.
function dedupByFilamentId(entries: FilamentEntry[]): FilamentEntry[] {
  const byId = new Map<string, FilamentEntry>();
  const unidentified: FilamentEntry[] = [];
  const score = (e: FilamentEntry) =>
    (typeof e.nozzle_temp_min === 'number' ? 1 : 0) +
    (typeof e.nozzle_temp_max === 'number' ? 1 : 0) +
    (typeof e.density === 'number' ? 1 : 0);
  for (const e of entries) {
    if (!e.filament_id) { unidentified.push(e); continue; }
    const prev = byId.get(e.filament_id);
    if (!prev || score(e) > score(prev)) byId.set(e.filament_id, e);
  }
  const out = [...byId.values(), ...unidentified];
  out.sort((a, b) => a.name.localeCompare(b.name));
  return out;
}

// Open the uploaded DB (if any), enumerate user-facing presets, and resolve
// each entry's picker-relevant fields. Cached for the lifetime of the tab:
// the DB is 1.7 MB and the resolve pass can run a few hundred ms on a slow
// machine — we only want to pay that once.
export function useFilamentsDb() {
  return useQuery({
    queryKey: ['filaments-db'],
    staleTime: Infinity,
    gcTime: Infinity,
    retry: false,
    queryFn: async () => {
      const res = await fetch('/api/filaments-db');
      if (res.status === 404) return { present: false as const, entries: [] };
      if (!res.ok) throw new Error(`GET /api/filaments-db failed: ${res.status}`);
      const buf = new Uint8Array(await res.arrayBuffer());
      const SQL = await loadSqlJs();
      const db = new SQL.Database(buf);
      try {
        // Only instantiable presets (the ones Bambu Studio shows in its
        // dropdown). Everything else is a base template that exists only
        // for inheritance. `filament_id` is NOT on these — it lives on
        // `@base` ancestors — so the resolver walks up to pick it up.
        const listed = db.exec(
          `SELECT name FROM filaments
             WHERE instantiation = 'true'
             ORDER BY name`,
        )[0];
        const rawEntries: FilamentEntry[] = [];
        listed?.values.forEach((row) => {
          const name = row[0] as string;
          rawEntries.push(entryFromResolved(name, resolveFilament(db, name)));
        });
        return { present: true as const, entries: dedupByFilamentId(rawEntries) };
      } finally {
        db.close();
      }
    },
  });
}

// ── Info endpoint (size + mtime) for the config page. Lightweight — 404
// means "no DB uploaded yet" and is surfaced as `present: false`. ─────
export interface FilamentsDbInfo {
  present: boolean;
  size?: number;
  mtime_s?: number;
}
export function useFilamentsDbInfo() {
  return useQuery<FilamentsDbInfo>({
    queryKey: ['filaments-db-info'],
    queryFn: () => fetch('/api/filaments-db/info').then((r) => r.json()),
    refetchInterval: 30_000,
  });
}

// ── Upload + delete helpers ──────────────────────────────────────────
export function useFilamentsDbUpload() {
  const [progress, setProgress] = useState(0);
  const qc = useQueryClient();
  const mutation = useMutation({
    mutationFn: (file: File) => uploadWithAuth('/api/upload/filaments-db', file, setProgress),
    onSettled: () => setProgress(0),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['filaments-db'] });
      qc.invalidateQueries({ queryKey: ['filaments-db-info'] });
    },
  });
  return { ...mutation, progress };
}

export function useFilamentsDbDelete() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () =>
      fetch('/api/filaments-db', { method: 'DELETE' }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'delete failed');
        return r.json();
      }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['filaments-db'] });
      qc.invalidateQueries({ queryKey: ['filaments-db-info'] });
    },
  });
}
