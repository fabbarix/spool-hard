import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { uploadWithAuth } from '@spoolhard/ui/utils/uploadWithAuth';
import { useState } from 'react';

// A single user-facing filament preset, flattened into the fields
// SpoolDetailPanel + the picker care about. Two sources, same shape:
//   - stock library (BambuStudio-derived; loaded from /api/filaments,
//     a flat JSONL pre-resolved by scripts/build_filaments_db.sh)
//   - user-managed presets from /api/user-filaments (CRUD'able,
//     optionally synced to Bambu Cloud)
// Stock entries carry a setting_id (the preset's full BambuStudio
// name); user entries carry PFUL<hash> for local-only or PFUS<hash>
// for cloud-synced. Either way the picker passes setting_id back to
// the spool record so the firmware can resolve the full preset later.
export interface FilamentEntry {
  name: string;            // raw preset name, e.g. "Bambu PLA Basic @base"
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
  pressure_advance?: number; // K; only useful when prefilling a custom filament
  base_id?: string;        // Bambu base preset id (e.g. "GFSA00")
  setting_id?: string;     // matches FilamentRecord::setting_id firmware-side
  source?: 'stock' | 'user';
}

// Wire shape the firmware emits — one row per line in filaments.jsonl,
// matching FilamentRecord (console/firmware/include/filament_record.h).
interface FilamentJsonRow {
  setting_id: string;
  stock?: boolean;
  name: string;
  base_id?: string;
  filament_type: string;
  filament_subtype?: string;
  filament_vendor: string;
  filament_id?: string;
  nozzle_temp_min: number;     // -1 = unset
  nozzle_temp_max: number;
  density: number;             // 0 = unset
  pressure_advance?: number;
}

function entryFromJsonRow(row: FilamentJsonRow): FilamentEntry {
  return {
    name:        row.name,
    filament_id: row.filament_id ?? '',
    brand:       row.filament_vendor ?? '',
    material:    row.filament_type ?? '',
    subtype:     row.filament_subtype ?? '',
    nozzle_temp_min: row.nozzle_temp_min > 0 ? row.nozzle_temp_min : undefined,
    nozzle_temp_max: row.nozzle_temp_max > 0 ? row.nozzle_temp_max : undefined,
    density:         row.density         > 0 ? row.density         : undefined,
    pressure_advance: row.pressure_advance && row.pressure_advance > 0
      ? row.pressure_advance : undefined,
    base_id:     row.base_id,
    setting_id:  row.setting_id,
    source:      'stock',
  };
}

// Open the uploaded library, parse line-by-line. Cached for the lifetime
// of the tab — the file's small (~32KB / ~100 rows) but parse + dedup
// still costs a few ms; one query keeps it sticky across tab switches.
export function useFilamentsDb() {
  return useQuery({
    queryKey: ['filaments-db'],
    staleTime: Infinity,
    gcTime: Infinity,
    retry: false,
    queryFn: async () => {
      const res = await fetch('/api/filaments');
      if (res.status === 404) return { present: false as const, entries: [] };
      if (!res.ok) throw new Error(`GET /api/filaments failed: ${res.status}`);
      const text = await res.text();
      const entries: FilamentEntry[] = [];
      for (const line of text.split('\n')) {
        const t = line.trim();
        if (!t) continue;
        try {
          const row = JSON.parse(t) as FilamentJsonRow;
          if (!row.setting_id || !row.name) continue;
          entries.push(entryFromJsonRow(row));
        } catch {
          // Skip malformed lines — a corrupt row shouldn't break the
          // whole library load.
        }
      }
      return { present: true as const, entries };
    },
  });
}

// ── Info endpoint (size + mtime) for the config page. Lightweight — 404
// means "no library uploaded yet" and is surfaced as `present: false`. ──
export interface FilamentsDbInfo {
  present: boolean;
  size?: number;
  mtime_s?: number;
}
export function useFilamentsDbInfo() {
  return useQuery<FilamentsDbInfo>({
    queryKey: ['filaments-db-info'],
    queryFn: () => fetch('/api/filaments/info').then((r) => r.json()),
    refetchInterval: 30_000,
  });
}

// ── Upload + delete helpers ──────────────────────────────────────────
export function useFilamentsDbUpload() {
  const [progress, setProgress] = useState(0);
  const qc = useQueryClient();
  const mutation = useMutation({
    mutationFn: (file: File) => uploadWithAuth('/api/upload/filaments', file, setProgress),
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
      fetch('/api/filaments', { method: 'DELETE' }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'delete failed');
        return r.json();
      }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['filaments-db'] });
      qc.invalidateQueries({ queryKey: ['filaments-db-info'] });
    },
  });
}
