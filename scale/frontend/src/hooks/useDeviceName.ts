import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { DeviceNameConfig } from '../types/api';

export function useDeviceName() {
  return useQuery<DeviceNameConfig>({
    queryKey: ['device-name'],
    queryFn: () => fetch('/api/device-name-config').then((r) => r.json()),
    staleTime: Infinity,
  });
}

export function useSetDeviceName() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: DeviceNameConfig) =>
      fetch('/api/device-name-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['device-name'] });
    },
  });
}
