import { useQuery } from '@tanstack/react-query';
import type { WifiNetwork } from '../types/api';

export function useWifiScan() {
  const query = useQuery<WifiNetwork[]>({
    queryKey: ['wifi-scan'],
    queryFn: async () => {
      const r = await fetch('/captive/api/wifi-scan');
      const data: WifiNetwork[] = await r.json();
      if (data.length === 0) {
        // Scan was just triggered — retry after a short delay
        await new Promise((res) => setTimeout(res, 2500));
        const r2 = await fetch('/captive/api/wifi-scan');
        return r2.json();
      }
      return data;
    },
    enabled: false,
  });
  return query;
}
