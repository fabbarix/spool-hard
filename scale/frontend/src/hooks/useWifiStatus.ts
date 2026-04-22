import { useQuery } from '@tanstack/react-query';
import type { WifiStatus } from '../types/api';

export function useWifiStatus() {
  return useQuery<WifiStatus>({
    queryKey: ['wifi-status'],
    queryFn: () => fetch('/api/wifi-status').then((r) => r.json()),
    refetchInterval: 15000,
  });
}
