import { useMutation, useQueryClient } from '@tanstack/react-query';

interface WifiConfig {
  ssid: string;
  pass: string;
  // Optional. Omit to leave existing NVS pin alone. Empty string clears
  // the pin. Non-empty must be "AA:BB:CC:DD:EE:FF" (case-insensitive).
  pinned_bssid?: string;
}

export function useSaveWifi() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: WifiConfig) =>
      fetch('/captive/api/wifi-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['wifi-status'] });
    },
  });
}
