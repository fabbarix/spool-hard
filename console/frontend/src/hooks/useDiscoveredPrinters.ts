import { useQuery } from '@tanstack/react-query';

export interface DiscoveredPrinter {
  serial: string;
  ip: string;
  model: string;
  last_seen_ago: number;   // ms
  configured: boolean;
}

export function useDiscoveredPrinters() {
  return useQuery<DiscoveredPrinter[]>({
    queryKey: ['discovery-printers'],
    queryFn: () => fetch('/api/discovery/printers').then((r) => r.json()),
    refetchInterval: 5000,
  });
}
