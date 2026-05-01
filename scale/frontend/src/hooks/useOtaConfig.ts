import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { OtaConfig, OtaStatus } from '../types/api';

const STATUS_KEY = ['ota-status'];
const CONFIG_KEY = ['ota-config'];

export function useOtaConfig() {
  return useQuery<OtaConfig>({
    queryKey: CONFIG_KEY,
    queryFn: () => fetch('/api/ota-config').then((r) => r.json()),
  });
}

export function useSetOtaConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<OtaConfig>) =>
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

// Push-driven via WS `state.ota` — fired periodically from the scale's
// main loop (rate-gated 5 s) and on every OTA mutation. `fast` is now a
// hint, kept for source compatibility; the actual cadence is bounded
// by the firmware's rate gate.
export function useOtaStatus(opts: { fast?: boolean } = {}) {
  return useQuery<OtaStatus>({
    queryKey: STATUS_KEY,
    queryFn: () => fetch('/api/ota-status').then((r) => r.json()),
    refetchInterval: opts.fast ? 5000 : 30000,   // safety poll only
    retry: false,
  });
}

export function useOtaCheckNow() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => fetch('/api/ota-check', { method: 'POST' }).then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: STATUS_KEY }),
  });
}

export function useRunOta() {
  return useMutation({
    mutationFn: () => fetch('/api/ota-run', { method: 'POST' }).then((r) => r.json()),
  });
}
