import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

// Reference-weight presets shown on the LCD's scale-calibration wizard.
// The user maintains the list here in the web UI; the LCD reads it
// fresh whenever the wizard opens (no reboot required). Stored in
// console NVS — one slot per console, not per scale.
export interface CalibrationPresetsConfig {
  presets: number[];
}

export function useCalibrationPresets() {
  return useQuery<CalibrationPresetsConfig>({
    queryKey: ['calibration-presets'],
    queryFn: () => fetch('/api/calibration-presets').then((r) => r.json()),
  });
}

export function useSaveCalibrationPresets() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: CalibrationPresetsConfig) =>
      fetch('/api/calibration-presets', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'save failed');
        return r.json();
      }),
    // Backend echoes the canonical (deduped, sorted, capped) list, so
    // we replace the cache straight from the response — no follow-up
    // GET round-trip.
    onSuccess: (data) => {
      qc.setQueryData(['calibration-presets'], data);
    },
  });
}
