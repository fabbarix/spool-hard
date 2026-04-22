import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

/** One pressure-advance calibration entry stored per (printer, nozzle, extruder). */
export interface KValueEntry {
  printer: string;        // Bambu printer serial
  nozzle: number;         // diameter in mm, e.g. 0.4
  extruder: number;       // 0 for X1/P1, 0/1 for H2D
  k: number;              // pressure-advance K
  cali_idx: number;       // Bambu calibration index; -1 = direct set
}

export interface SpoolExt {
  k_values?: KValueEntry[];
  // Other ext fields (origin_data, raw tag payload, etc.) may appear here.
}

export interface SpoolRecord {
  id: string;
  tag_id: string;
  material_type: string;
  material_subtype?: string;
  color_name?: string;
  color_code?: string;
  brand?: string;
  weight_advertised?: number;
  weight_core?: number;
  weight_new?: number;
  weight_current?: number;
  consumed_since_add?: number;
  consumed_since_weight?: number;
  // Print settings. -1 (or omitted) → firmware falls back to material-default
  // temps at ams_filament_setting push time.
  nozzle_temp_min?: number;
  nozzle_temp_max?: number;
  // Filament density in g/cm³. Consumed by the gcode analyzer on the
  // firmware side when converting extruded mm → grams for print-consume
  // tracking. 0 or omitted → analyzer uses a per-material-family fallback.
  density?: number;
  // Bambu tray_info_idx (e.g. "GFL99"). Matches the slicer_filament field in
  // yanshay/SpoolEase's SpoolRecord; sent verbatim on AMS assignment push.
  slicer_filament?: string;
  note?: string;
  data_origin?: string;
  tag_type?: string;
  ext?: SpoolExt;
}

export interface SpoolsPage {
  total: number;
  offset: number;
  limit: number;
  rows: SpoolRecord[];
}

export function useSpools(offset = 0, limit = 50, material?: string) {
  const params = new URLSearchParams({ offset: String(offset), limit: String(limit) });
  if (material) params.set('material', material);
  return useQuery<SpoolsPage>({
    queryKey: ['spools', offset, limit, material],
    queryFn: () => fetch(`/api/spools?${params}`).then((r) => r.json()),
    refetchInterval: 4000,
  });
}

export function useSpoolUpsert() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (rec: Partial<SpoolRecord>) =>
      fetch('/api/spools', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(rec),
      }).then((r) => r.json()),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['spools'] });
      qc.invalidateQueries({ queryKey: ['spool'] });
    },
  });
}

export function useSpoolDelete() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: string) =>
      fetch(`/api/spools/${encodeURIComponent(id)}`, { method: 'DELETE' }).then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['spools'] }),
  });
}
