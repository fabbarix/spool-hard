import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { OtaConfig } from '../types/api';

export function useOtaConfig() {
  return useQuery<OtaConfig>({
    queryKey: ['ota-config'],
    queryFn: () => fetch('/api/ota-config').then((r) => r.json()),
  });
}

export function useSetOtaConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: OtaConfig) =>
      fetch('/api/ota-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['ota-config'] });
    },
  });
}
