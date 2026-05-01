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
    // Push-driven via WS `state.discovery_printers` — fired from
    // BambuDiscovery::setOnSeen on every NOTIFY/M-SEARCH match plus
    // a periodic 1 Hz tick. Rate-gated 2s on the firmware side.
  });
}
