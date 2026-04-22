import { useQuery } from '@tanstack/react-query';

export interface WifiStatus {
  configured: boolean;
  connected: boolean;
  ssid: string;
  ip: string;
  rssi: number;
}

export function useWifiStatus() {
  return useQuery<WifiStatus>({
    queryKey: ['wifi-status'],
    queryFn: () => fetch('/api/wifi-status').then((r) => r.json()),
    refetchInterval: 5000,
  });
}
