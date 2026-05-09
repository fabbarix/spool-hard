import { useQuery } from '@tanstack/react-query';

export interface WifiStatus {
  configured: boolean;
  connected: boolean;
  ssid: string;
  ip: string;
  rssi: number;
  bssid: string;
  channel: number;
  // Persisted user pin: target BSSID for WiFi.begin, or "" for auto.
  // Survives reboot. Cleared in RAM after a 60 s fallback if the
  // pinned node is unreachable, but NVS keeps the user's intent.
  pinned_bssid: string;
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
