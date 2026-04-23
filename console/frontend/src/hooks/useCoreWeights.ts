import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface CoreWeightEntry {
  brand: string;
  material: string;
  advertised: number;    // grams of filament (as packaged)
  grams: number;         // measured empty-core weight
  updated_ms: number;    // millis() on the device when it was last written
}

// Composite key matching the firmware (CoreWeights::keyFor). DELETE takes
// it as a query param, so we pre-compute here.
export const coreKey = (e: Pick<CoreWeightEntry, 'brand' | 'material' | 'advertised'>) =>
  `${e.brand}/${e.material}/${e.advertised}`;

export function useCoreWeights() {
  // Poll every 4s so entries learned by the LCD wizard surface in the
  // web UI without a manual refresh. The wizard writes straight to NVS
  // (CoreWeights::set) and has no way to invalidate the React Query
  // cache from across the firmware/browser boundary, so the only
  // alternatives are this poll or wiring a /ws "core-weights changed"
  // broadcast — overkill for a table this small.
  return useQuery<CoreWeightEntry[]>({
    queryKey: ['core-weights'],
    queryFn: () => fetch('/api/core-weights').then((r) => r.json()),
    refetchInterval: 4000,
  });
}

export function useUpsertCoreWeight() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Omit<CoreWeightEntry, 'updated_ms'>) =>
      fetch('/api/core-weights', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'save failed');
        return r.json();
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['core-weights'] }),
  });
}

export function useDeleteCoreWeight() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (key: string) =>
      fetch(`/api/core-weights?key=${encodeURIComponent(key)}`, { method: 'DELETE' })
        .then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['core-weights'] }),
  });
}
