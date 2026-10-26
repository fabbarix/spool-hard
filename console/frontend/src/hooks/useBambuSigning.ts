import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

// Bambu MQTT command signing identity (firmware: bambu_signer.h). The key
// is write-only; the status never carries PEM material.
export interface BambuSigningStatus {
  configured: boolean;
  enabled: boolean;
  cert_id?: string;
  subject?: string;
  issuer?: string;
  not_before?: string;   // YYYY-MM-DD
  not_after?: string;
  crl_present?: boolean;
  crl_next_update?: string;
  revoked?: boolean;
  chain_certs?: number;  // certificates in the stored cert file (leaf + CAs)
  error?: string;        // stored files present but unusable
}

export type BambuSigningBody =
  | { cert_pem: string; key_pem: string; crl_pem?: string }
  | { enabled: boolean };

async function send(method: string, body?: BambuSigningBody): Promise<BambuSigningStatus> {
  const r = await fetch('/api/bambu-signing', {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error ?? `request failed (${r.status})`);
  return j as BambuSigningStatus;
}

export function useBambuSigning() {
  return useQuery<BambuSigningStatus>({
    queryKey: ['bambu-signing'],
    queryFn: () => fetch('/api/bambu-signing').then((r) => r.json()),
  });
}

export function useSaveBambuSigning() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: BambuSigningBody) => send('POST', body),
    onSuccess: (st) => qc.setQueryData(['bambu-signing'], st),
  });
}

export function useRemoveBambuSigning() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => send('DELETE'),
    onSuccess: (st) => qc.setQueryData(['bambu-signing'], st),
  });
}

// Live test against a printer (firmware: BambuPrinter::requestSignTest):
// a signed ams_filament_setting that re-sends one loaded slot's current
// values, so the printer's answer proves the signature without changing it.
export type SignTestState =
  'idle' | 'queued' | 'registering' | 'sent' | 'accepted' | 'refused' | 'failed' | 'timeout';

export interface SignTestResult {
  serial: string;
  state: SignTestState;
  slot: string;         // "AMS 1 slot 2" | "External" | ""
  reason: string;
  err: number;
  elapsed_ms: number;
}

const testUrl = (serial: string) =>
  `/api/bambu-signing/printer-test?serial=${encodeURIComponent(serial)}`;

export function useSignTest(serial: string | undefined) {
  return useQuery<SignTestResult>({
    queryKey: ['bambu-signing', 'printer-test', serial],
    enabled: !!serial,
    queryFn: async () => {
      const r = await fetch(testUrl(serial!));
      const j = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(j.error ?? `request failed (${r.status})`);
      return j as SignTestResult;
    },
    // Poll only while the printer's answer is outstanding.
    refetchInterval: (q) =>
      ['queued', 'registering', 'sent'].includes(q.state.data?.state ?? '') ? 700 : false,
  });
}

export function useStartSignTest() {
  const qc = useQueryClient();
  return useMutation<SignTestResult, Error, string>({
    mutationFn: async (serial) => {
      const r = await fetch(testUrl(serial), { method: 'POST' });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(j.error ?? `request failed (${r.status})`);
      return j as SignTestResult;
    },
    onSuccess: (res, serial) =>
      qc.setQueryData(['bambu-signing', 'printer-test', serial], res),
  });
}
