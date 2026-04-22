import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { ScaleConfig } from '../types/api';

export function useScaleConfig() {
  return useQuery<ScaleConfig>({
    queryKey: ['scale-config'],
    queryFn: () => fetch('/api/scale-config').then((r) => r.json()),
  });
}

export function useSetScaleConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<ScaleConfig>) =>
      fetch('/api/scale-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }).then((r) => r.json()),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['scale-config'] });
    },
  });
}

export function useTare() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () =>
      fetch('/api/scale-tare', { method: 'POST' })
        .then((r) => r.json()) as Promise<{ ok: boolean; tare_raw?: number }>,
    onSuccess: (res) => {
      // The firmware returns the fresh tare_raw so we can patch the cache
      // immediately — avoids the ~200 ms flicker where the UI still shows
      // the old zero while the background GET /api/scale-config completes.
      if (typeof res?.tare_raw === 'number') {
        qc.setQueryData<ScaleConfig | undefined>(['scale-config'], (old) =>
          old ? { ...old, tare_raw: res.tare_raw! } : old,
        );
      }
      // Follow up with an invalidation anyway — picks up anything else
      // that may have shifted (e.g. `calibrated` flag transitions).
      qc.invalidateQueries({ queryKey: ['scale-config'] });
    },
  });
}

export function useCalibrate() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (weight: number) =>
      fetch('/api/scale-calibrate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ weight }),
      }).then((r) => r.json()),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['scale-config'] });
    },
  });
}

export function useAddCalPoint() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (weight: number) =>
      fetch('/api/scale-cal-point', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ weight }),
      }).then((r) => r.json()),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['scale-config'] });
    },
  });
}

export function useClearCal() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => fetch('/api/scale-cal-clear', { method: 'POST' }).then((r) => r.json()),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['scale-config'] });
    },
  });
}

export function useCaptureRaw() {
  return useMutation({
    mutationFn: () => fetch('/api/scale-raw').then((r) => r.json()) as Promise<{ raw: number }>,
  });
}
