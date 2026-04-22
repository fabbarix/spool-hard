import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface AmsTray {
  id: number;
  material: string;
  color: string;            // "RRGGBBAA"
  tray_info_idx: string;    // Bambu material code
  tag_uid: string;
  spool_id: string;         // resolved against local SpoolStore (empty if unknown)
  spool_override: boolean;  // true when the mapping came from a user override
                            // rather than automatic tag_uid matching
  remain_pct: number;       // -1 if unknown
  nozzle_min_c: number;
  nozzle_max_c: number;
  k: number;                // pressure-advance K (0 if unset)
  cali_idx: number;         // Bambu calibration index; -1 when direct-set
}

export interface AmsUnit {
  id: number;
  humidity: number;
  trays: AmsTray[];
}

export interface PrinterState {
  link: 'connected' | 'connecting' | 'disconnected' | 'failed';
  gcode_state?: string;
  progress_pct?: number;
  layer_num?: number;
  total_layers?: number;
  bed_temp?: number;
  nozzle_temp?: number;
  active_tray?: number;
  last_report_ms_ago?: number;
  error?: string;
  ams?: AmsUnit[];
  // External spool holder (single-extruder printers). Bambu calls this
  // `vt_tray`; id is typically 254. Absent on printers that don't report one.
  vt_tray?: AmsTray;
}

export interface Printer {
  name: string;
  serial: string;
  ip: string;
  access_code_preview?: string;
  auto_restore_k?: boolean;
  track_print_consume?: boolean;
  state: PrinterState;
}

export function usePrinters() {
  return useQuery<Printer[]>({
    queryKey: ['printers'],
    queryFn: () => fetch('/api/printers').then((r) => r.json()),
    refetchInterval: 3000,
  });
}

export function useUpsertPrinter() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: { name: string; serial: string; ip: string; access_code: string }) =>
      fetch('/api/printers', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json()).error ?? 'save failed');
        return r.json();
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['printers'] }),
  });
}

export function useDeletePrinter() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (serial: string) =>
      fetch(`/api/printers/${encodeURIComponent(serial)}`, { method: 'DELETE' }).then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['printers'] }),
  });
}

export interface AnalysisTool {
  tool_idx: number;
  grams: number;           // total predicted for this tool over the whole print
  mm: number;
  ams_unit: number;
  slot_id: number;
  spool_id: string;
  material: string;
  color: string;
  grams_consumed: number;  // live: grams extruded so far (backend uses M73 table)
}

export interface GcodeAnalysis {
  in_progress: boolean;
  valid: boolean;
  path: string;
  error: string;
  started_ms: number;
  finished_ms: number;
  total_grams: number;
  total_mm: number;
  has_pct_table: boolean;  // true when backend has per-% exact consumption data
  progress_pct: number;    // 0..100, last value reported by the printer
  gcode_state: string;     // "IDLE" | "PREPARE" | "RUNNING" | "PAUSE" | "FINISH" | "FAILED"
  tools: AnalysisTool[];
}

export function usePrinterAnalysis(serial: string | undefined) {
  return useQuery<GcodeAnalysis>({
    queryKey: ['printer-analysis', serial],
    queryFn: () =>
      fetch(`/api/printers/${encodeURIComponent(serial!)}/analysis`).then((r) => r.json()),
    enabled: !!serial,
    // Poll fast while the fetch+parse is running OR while the printer is
    // actively printing (progress_pct advances), so the live "grams
    // consumed" counter tracks reality with minimal lag.
    refetchInterval: (q) => {
      const a = q.state.data;
      if (!a) return false;
      if (a.in_progress) return 1500;
      const active = a.gcode_state === 'RUNNING' || a.gcode_state === 'PAUSE';
      return active ? 3000 : false;
    },
  });
}

export function useSetAmsMapping() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ serial, ams_unit, slot_id, spool_id }: {
      serial: string; ams_unit: number; slot_id: number; spool_id: string;
    }) =>
      fetch(`/api/printers/${encodeURIComponent(serial)}/ams-mapping`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ams_unit, slot_id, spool_id }),
      }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'save failed');
        return r.json();
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['printers'] }),
  });
}

export function useStartPrinterAnalysis() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ serial, path }: { serial: string; path?: string }) =>
      fetch(`/api/printers/${encodeURIComponent(serial)}/analyze`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(path ? { path } : {}),
      }).then(async (r) => {
        if (!r.ok && r.status !== 202)
          throw new Error((await r.json().catch(() => ({}))).error ?? 'start failed');
        return r.json();
      }),
    onSuccess: (_data, vars) =>
      qc.invalidateQueries({ queryKey: ['printer-analysis', vars.serial] }),
  });
}
