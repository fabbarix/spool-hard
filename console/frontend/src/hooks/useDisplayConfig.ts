import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface DisplayConfig {
  /** Seconds of idle time before the screen turns off. 0 = never sleep. */
  sleep_timeout_s: number;
}

export function useDisplayConfig() {
  return useQuery<DisplayConfig>({
    queryKey: ['display-config'],
    queryFn: () => fetch('/api/display-config').then((r) => r.json()),
  });
}

export function useSaveDisplayConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (cfg: DisplayConfig) =>
      fetch('/api/display-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cfg),
      }).then(async (r) => {
        if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error ?? 'save failed');
        return r.json() as Promise<DisplayConfig & { ok: boolean }>;
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['display-config'] }),
  });
}
