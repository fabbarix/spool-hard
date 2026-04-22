import { useQuery } from '@tanstack/react-query';

export interface WifiNetwork { ssid: string; rssi: number; secure: boolean; }

export function useWifiScan() {
  return useQuery<WifiNetwork[]>({
    queryKey: ['wifi-scan'],
    queryFn: async () => {
      const r = await fetch('/captive/api/wifi-scan');
      const data: WifiNetwork[] = await r.json();
      if (data.length === 0) {
        await new Promise((res) => setTimeout(res, 2500));
        const r2 = await fetch('/captive/api/wifi-scan');
        return r2.json();
      }
      return data;
    },
    enabled: false,
  });
}
