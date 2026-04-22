import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface ScaleLastEvent {
  kind: string;         // "weight" | "tag" | "button" | "uncalibrated" | "version" | ""
  detail: string;
  scale_name: string;   // which scale emitted the event (may be "" if none yet)
  ago_ms: number;       // -1 if no event yet
}

export interface ScaleLastWeight {
  grams: number;        // last value received; 0 if nothing yet
  state: string;        // "new" | "stable" | "unstable" | "removed" | "uncalibrated" | ""
  ago_ms: number;       // -1 if nothing yet
  precision?: number;   // scale's configured decimal places (0..4); falls back to 0
}

export interface ScaleLinkStatus {
  connected: boolean;
  handshake?: 'encrypted' | 'unencrypted' | 'failed' | 'disconnected';
  ip?: string;
  name?: string;
  last_event?: ScaleLastEvent;
  weight?: ScaleLastWeight;
}

export function useScaleLink() {
  return useQuery<ScaleLinkStatus>({
    queryKey: ['scale-link'],
    queryFn: () => fetch('/api/scale-link').then((r) => r.json()),
    refetchInterval: 2000,
  });
}

export function useScaleLinkTare() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => fetch('/api/scale-link/tare', { method: 'POST' }).then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['scale-link'] }),
  });
}

export interface ScaleSecretStatus {
  name: string;
  configured: boolean;
  preview: string;
}

/** Secret configured for a specific scale, keyed by name. */
export function useScaleSecret(name: string | undefined) {
  return useQuery<ScaleSecretStatus>({
    queryKey: ['scale-secret', name],
    queryFn: () =>
      fetch(`/api/scale-secret?name=${encodeURIComponent(name!)}`).then((r) => r.json()),
    enabled: !!name,
  });
}

export function useSetScaleSecret() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ name, secret }: { name: string; secret: string }) =>
      fetch('/api/scale-secret', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, secret }),
      }).then((r) => r.json()),
    onSuccess: (_, { name }) => {
      qc.invalidateQueries({ queryKey: ['scale-secret', name] });
      qc.invalidateQueries({ queryKey: ['discovery-scales'] });
      qc.invalidateQueries({ queryKey: ['scale-link'] });
    },
  });
}
