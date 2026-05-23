import { useQuery, useMutation } from '@tanstack/react-query';

export interface LedState {
  id: string;
  label: string;
  kind: 'solid' | 'flash' | 'pulse' | 'burst';
  color: string;       // #rrggbb, dimmed to match the LED
  period_ms?: number;  // present for flash + pulse
  desc: string;
}

interface LedLegend {
  states: LedState[];
}

export function useLedLegend() {
  return useQuery<LedLegend>({
    queryKey: ['led-legend'],
    queryFn: () => fetch('/api/led-legend').then((r) => r.json()),
    // The legend is firmware-baked metadata — fetch once per session.
    staleTime: Infinity,
  });
}

export function useLedTest() {
  return useMutation({
    mutationFn: ({ id, ms = 5000 }: { id: string; ms?: number }) => {
      const params = new URLSearchParams({ state: id, ms: String(ms) });
      return fetch(`/api/led-test?${params.toString()}`, { method: 'POST' })
        .then((r) => r.json());
    },
  });
}
