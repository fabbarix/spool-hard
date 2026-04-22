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

// Periodic-check telemetry. Polled every 5 s while the page is open so the
// in-flight indicator and the post-check status flip live in the UI.
export function useOtaStatus() {
  return useQuery<OtaStatus>({
    queryKey: STATUS_KEY,
    queryFn: () => fetch('/api/ota-status').then((r) => r.json()),
    refetchInterval: 5000,
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
