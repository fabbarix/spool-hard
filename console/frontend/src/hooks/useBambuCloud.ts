import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

// Mirror of the GET /api/bambu-cloud response. `expires_at` is the
// Unix timestamp pulled from the JWT's `exp` claim — 0 means we
// couldn't decode it (token isn't a JWT, or claim was missing).
export interface BambuCloudStatus {
  configured: boolean;
  region: 'global' | 'china';
  email: string;
  token?: string;
  token_preview?: string;
  expires_at?: number;
}

// Raw response details, populated on every non-Ok step result. Powers
// the "show details" collapsible in the UI when the friendly message
// isn't enough to figure out what the API actually returned.
export interface BambuStepDiagnostics {
  request_url: string;
  http_status: number;
  response_headers: string;
  response_body: string;
}

// Returned by every login-step endpoint. The frontend reads `status`
// to decide whether to show the email-code field, the TFA-code field,
// or stop here as success/failure. `tfa_key` is only set when status
// is "need_tfa" — must be passed back into the next call.
// `diagnostics` is present on every non-`ok` outcome.
export interface BambuLoginStepResult {
  status: 'ok' | 'need_email_code' | 'need_tfa' |
          'invalid_credentials' | 'network_error' | 'server_error';
  tfa_key?: string;
  message?: string;
  diagnostics?: BambuStepDiagnostics;
}

export type BambuRegion = 'global' | 'china';

const STATUS_KEY = ['bambu-cloud'];

export function useBambuCloud() {
  return useQuery<BambuCloudStatus>({
    queryKey: STATUS_KEY,
    queryFn: () => fetch('/api/bambu-cloud').then((r) => r.json()),
  });
}

// All four mutations share a similar shape: POST JSON, get back a
// step result. They invalidate the status query on success so the
// "configured?" UI flips immediately.
function postJson<TBody, TRes>(path: string, body: TBody): Promise<TRes> {
  return fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  }).then(async (r) => {
    const json = await r.json();
    return json as TRes;
  });
}

export function useBambuLoginPassword() {
  const qc = useQueryClient();
  return useMutation<BambuLoginStepResult, Error, { account: string; password: string; region: BambuRegion }>({
    mutationFn: (b) => postJson('/api/bambu-cloud/login', b),
    onSuccess: (res) => {
      if (res.status === 'ok') qc.invalidateQueries({ queryKey: STATUS_KEY });
    },
  });
}

export function useBambuLoginCode() {
  const qc = useQueryClient();
  return useMutation<BambuLoginStepResult, Error, { account: string; code: string; region: BambuRegion }>({
    mutationFn: (b) => postJson('/api/bambu-cloud/login-code', b),
    onSuccess: (res) => {
      if (res.status === 'ok') qc.invalidateQueries({ queryKey: STATUS_KEY });
    },
  });
}

export function useBambuLoginTfa() {
  const qc = useQueryClient();
  return useMutation<BambuLoginStepResult, Error, { tfa_key: string; tfa_code: string; account: string; region: BambuRegion }>({
    mutationFn: (b) => postJson('/api/bambu-cloud/login-tfa', b),
    onSuccess: (res) => {
      if (res.status === 'ok') qc.invalidateQueries({ queryKey: STATUS_KEY });
    },
  });
}

export function useBambuSetToken() {
  const qc = useQueryClient();
  return useMutation<{ status: string; message?: string }, Error, { token: string; email?: string; region: BambuRegion }>({
    mutationFn: (b) => postJson('/api/bambu-cloud/token', b),
    onSuccess: (res) => {
      if (res.status === 'ok') qc.invalidateQueries({ queryKey: STATUS_KEY });
    },
  });
}

export function useBambuVerify() {
  const qc = useQueryClient();
  return useMutation<{ valid: boolean }, Error, void>({
    mutationFn: () =>
      fetch('/api/bambu-cloud/verify', { method: 'POST' }).then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: STATUS_KEY }),
  });
}

export function useBambuClear() {
  const qc = useQueryClient();
  return useMutation<{ ok: boolean }, Error, void>({
    mutationFn: () =>
      fetch('/api/bambu-cloud', { method: 'DELETE' }).then((r) => r.json()),
    onSuccess: () => qc.invalidateQueries({ queryKey: STATUS_KEY }),
  });
}
