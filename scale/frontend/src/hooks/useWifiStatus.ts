import { useQuery } from '@tanstack/react-query';
import type { WifiStatus } from '../types/api';

export function useWifiStatus() {
  return useQuery<WifiStatus>({
    queryKey: ['wifi-status'],
    queryFn: () => fetch('/api/wifi-status').then((r) => r.json()),
    // Push-driven via WS `state.wifi_status` from the scale's main loop
    // (rate-gated 30 s on the firmware side).
  });
}
