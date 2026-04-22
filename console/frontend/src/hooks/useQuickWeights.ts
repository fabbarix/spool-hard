import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface QuickWeightsConfig {
  grams: number[];
}

export function useQuickWeights() {
  return useQuery<QuickWeightsConfig>({
    queryKey: ['quick-weights'],
    queryFn: () => fetch('/api/quick-weights').then((r) => r.json()),
  });
}

export function useSaveQuickWeights() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: QuickWeightsConfig) =>
      fetch('/api/quick-weights', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'save failed');
        return r.json();
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['quick-weights'] }),
  });
}
