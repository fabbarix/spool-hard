import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface OtaConfigT {
  url: string;
  use_ssl: boolean;
  verify_ssl: boolean;
}

export function useOtaConfig() {
  return useQuery<OtaConfigT>({
    queryKey: ['ota-config'],
    queryFn: () => fetch('/api/ota-config').then((r) => r.json()),
  });
}

export function useSetOtaConfig() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: OtaConfigT) =>
      fetch('/api/ota-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['ota-config'] }),
  });
}

export function useRunOta() {
  return useMutation({
    mutationFn: () => fetch('/api/ota-run', { method: 'POST' }).then((r) => r.json()),
  });
}
