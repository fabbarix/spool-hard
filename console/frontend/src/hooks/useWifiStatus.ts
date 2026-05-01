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
    // Push-driven via WS `state.wifi_status` — rate-gated 30s on the
    // firmware side so RSSI fluctuations don't flood. Header pill +
    // Setup tab read this; both tolerate ~30s staleness.
  });
}
