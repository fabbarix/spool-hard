import { useQuery } from '@tanstack/react-query';

export type ScaleHandshake = 'encrypted' | 'unencrypted' | 'failed' | 'disconnected';

export interface DiscoveredScale {
  name: string;
  ip: string;
  last_seen_ago: number;
  paired: boolean;
  connected: boolean;
  /** Link crypto state of the paired scale (only meaningful when paired=true).
   *  encrypted → green, unencrypted/failed → amber, disconnected → red. */
  handshake: ScaleHandshake;
}

export function useDiscoveredScales() {
  return useQuery<DiscoveredScale[]>({
    queryKey: ['discovery-scales'],
    queryFn: () => fetch('/api/discovery/scales').then((r) => r.json()),
    refetchInterval: 5000,
  });
}
