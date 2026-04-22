import { useMutation, useQueryClient } from '@tanstack/react-query';

export function useSaveWifi() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: { ssid: string; pass: string }) =>
      fetch('/captive/api/wifi-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['wifi-status'] }),
  });
}
