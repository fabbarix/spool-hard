import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

// Firmware-side session-only debug toggles. Nothing persists across reboots
// by design — leaving AMS logging on would chew ~1–5 KB of WebSocket
// traffic every couple of seconds.
export interface DebugConfig {
  log_ams: boolean;
}

export function useDebugConfig() {
  return useQuery<DebugConfig>({
    queryKey: ['debug-config'],
    queryFn: () => fetch('/api/debug/config').then((r) => r.json()),
    // The flag's value is meaningful only while the user is actively
    // watching the log — no need to refetch in the background.
    refetchInterval: false,
    staleTime: Infinity,
  });
}

export function useUpdateDebugConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (patch: Partial<DebugConfig>) =>
      fetch('/api/debug/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(patch),
      }).then(async (r) => {
        if (!r.ok) throw new Error(`debug/config: ${r.status}`);
        return r.json() as Promise<DebugConfig>;
      }),
    onSuccess: (data) => {
      qc.setQueryData(['debug-config'], data);
    },
  });
}
