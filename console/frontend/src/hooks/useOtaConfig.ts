import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

// /api/ota-config payload (read + write).
export interface OtaConfigT {
  url: string;
  use_ssl: boolean;
  verify_ssl: boolean;
  check_enabled: boolean;
  check_interval_h: number;   // 1 / 6 / 24 / 168
}

// /api/ota-status — what the periodic checker knows. The `scale` block
// is populated from the cached OtaPending frame the scale pushes over
// the WS link (Phase 5). When the link is down, only `link` and
// `pending: false` are present.
export interface OtaPendingProductT {
  firmware_current: string;
  firmware_latest: string;
  frontend_current: string;
  frontend_latest: string;
  pending: boolean;
}
export type ScaleLinkState = 'online' | 'waiting' | 'offline';
export interface OtaStatusScaleT extends Partial<OtaPendingProductT> {
  link: ScaleLinkState;
  pending: boolean;
  last_check_ts?: number;
  last_check_status?: string;
}
export interface OtaStatusT {
  check_enabled: boolean;
  check_interval_h: number;
  last_check_ts: number;             // Unix seconds; 0 = never
  last_check_status: string;         // "" | "ok" | "network" | "http_error" | "parse_error"
  check_in_flight: boolean;
  console: OtaPendingProductT;
  scale: OtaStatusScaleT;
}

const STATUS_KEY = ['ota-status'];
const CONFIG_KEY = ['ota-config'];

export function useOtaConfig() {
  return useQuery<OtaConfigT>({
    queryKey: CONFIG_KEY,
    queryFn: () => fetch('/api/ota-config').then((r) => r.json()),
  });
}

export function useSetOtaConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<OtaConfigT>) =>
      fetch('/api/ota-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }).then((r) => r.json()),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: CONFIG_KEY });
      qc.invalidateQueries({ queryKey: STATUS_KEY });
    },
  });
}

// Periodic-check telemetry. Polled every 5 s while the page is open
// so the in-flight indicator and the post-check status flip live.
export function useOtaStatus() {
  return useQuery<OtaStatusT>({
    queryKey: STATUS_KEY,
    queryFn: () => fetch('/api/ota-status').then((r) => r.json()),
    refetchInterval: 5000,
  });
}

export function useOtaCheckNow() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => fetch('/api/ota-check', { method: 'POST' }).then((r) => r.json()),
    // Status will flip to in_flight on the next poll, then settle.
    onSuccess: () => qc.invalidateQueries({ queryKey: STATUS_KEY }),
  });
}

export function useRunOta() {
  return useMutation({
    mutationFn: () => fetch('/api/ota-run', { method: 'POST' }).then((r) => r.json()),
  });
}

// Phase 5: trigger an OTA on the paired scale. Goes through the console
// because the browser can't reach the scale's web UI from the console-page
// origin (CORS + auth-key live on the console). Returns 503 if the scale
// link is down — the UI catches that and surfaces a clear message.
export function useRunOtaScale() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async () => {
      const r = await fetch('/api/ota-update-scale', { method: 'POST' });
      const j = await r.json();
      if (!r.ok) throw new Error(j?.error || `HTTP ${r.status}`);
      return j;
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: STATUS_KEY }),
  });
}
