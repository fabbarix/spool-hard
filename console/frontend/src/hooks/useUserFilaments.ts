import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

// Mirrors the firmware's FilamentRecord (console/firmware/include/filament_record.h).
// Keep field names in sync — server dumps these verbatim from the JSONL on disk.
export interface PaByNozzleEntry {
  nozzle: number;            // mm, e.g. 0.4
  k: number;                 // pressure-advance K
}

export interface UserFilament {
  setting_id: string;        // PFUL<hash> for local; PFUS<hash> for cloud-sourced
  stock?: boolean;           // false for user records; not surfaced for stock
  name: string;
  base_id: string;           // Bambu's parent preset id, e.g. "GFSA00"
  filament_type: string;     // PLA / PETG / TPU / ...
  filament_subtype?: string; // basic / matte / translucent / ...
  filament_vendor: string;
  filament_id?: string;      // Bambu's tray_info_idx, e.g. "GFL99"
  nozzle_temp_min: number;   // -1 = unset
  nozzle_temp_max: number;
  density: number;           // 0 = unset
  // Pressure-advance: a default scalar (cloud-roundtripped) plus an
  // optional per-nozzle list. PA depends on (filament, nozzle) so the
  // list is the preferred source; the scalar is the fallback for any
  // nozzle without an explicit entry.
  pressure_advance: number;
  pa_by_nozzle?: PaByNozzleEntry[];
  cloud_setting_id: string;  // "" until pushed; PFUS<hash> after sync
  cloud_synced_at: number;   // epoch s
  updated_at: number;        // epoch s — drives "out of sync" badge
}

export interface UserFilamentsList {
  total: number;
  offset: number;
  limit: number;
  rows: UserFilament[];
}

const LIST_KEY = ['user-filaments'];

export function useUserFilaments(material?: string) {
  return useQuery<UserFilamentsList>({
    queryKey: [...LIST_KEY, material ?? ''],
    queryFn: () => {
      const qs = material ? `?material=${encodeURIComponent(material)}` : '';
      return fetch(`/api/user-filaments${qs}`).then((r) => r.json());
    },
  });
}

export function useUpsertUserFilament() {
  const qc = useQueryClient();
  return useMutation<UserFilament, Error, Partial<UserFilament>>({
    mutationFn: async (body) => {
      // POST creates (server generates the setting_id). PUT updates by id.
      const isUpdate = !!body.setting_id;
      const url = isUpdate
        ? `/api/user-filaments/${encodeURIComponent(body.setting_id!)}`
        : '/api/user-filaments';
      const r = await fetch(url, {
        method: isUpdate ? 'PUT' : 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      const j = await r.json();
      if (!r.ok) throw new Error(j?.error || `HTTP ${r.status}`);
      return j as UserFilament;
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: LIST_KEY }),
  });
}

export function useDeleteUserFilament() {
  const qc = useQueryClient();
  return useMutation<void, Error, string>({
    mutationFn: async (id) => {
      const r = await fetch(`/api/user-filaments/${encodeURIComponent(id)}`, {
        method: 'DELETE',
      });
      if (!r.ok) {
        const j = await r.json().catch(() => ({}));
        throw new Error(j?.error || `HTTP ${r.status}`);
      }
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: LIST_KEY }),
  });
}

// Diagnostics block included by the firmware on any non-ok cloud
// outcome. Mirrors the BambuCloudFilaments::Diag struct on the
// firmware side; the React UI surfaces it in a collapsible "show
// details" panel so the user can see WHY a sync / push failed.
export interface CloudDiagnostics {
  stage: string;          // "list", "fetchOne <id>", "create", "delete <id>", ...
  request_url: string;
  http_status: number;    // 0 = transport never landed
  cf_blocked: boolean;
  response_body: string;  // truncated to ~512 bytes
}

// Cloud sync: pull every preset the user has on Bambu Cloud into the
// local user_filaments store. Runs as a background task on the device
// so the AsyncTCP server stays responsive — the POST returns
// immediately with phase:"running", then we poll
// `/cloud-sync/status` until phase:"done". Soft-fails on a Cloudflare
// WAF block (status === 'unreachable') — local list isn't disturbed.
export interface CloudSyncResult {
  // Final status when phase==='done'. While running, only `phase` is
  // populated.
  phase:        'idle' | 'running' | 'done';
  started_at?:  number;
  finished_at?: number;
  status?:      'ok' | 'rejected' | 'unreachable';
  added?:       number;
  updated?:     number;
  diagnostics?: CloudDiagnostics;
}
export function useCloudSyncFilaments() {
  const qc = useQueryClient();
  return useMutation<CloudSyncResult, Error, void>({
    mutationFn: async () => {
      // Kick off the background sync. The POST returns 202 + status
      // immediately; the actual cloud library walk runs on a FreeRTOS
      // task. We then poll `/cloud-sync/status` every 1 s until the
      // device reports `phase:"done"`. Cap the wait at 5 minutes to
      // avoid hanging forever if the device drops off the network.
      const start = await fetch('/api/user-filaments/cloud-sync', { method: 'POST' });
      const startJson = await start.json();
      if (!start.ok) throw new Error(startJson?.error || `HTTP ${start.status}`);

      const deadline = Date.now() + 5 * 60 * 1000;
      while (Date.now() < deadline) {
        await new Promise((r) => setTimeout(r, 1000));
        const r = await fetch('/api/user-filaments/cloud-sync/status');
        if (!r.ok) continue;   // transient — keep polling
        const j = (await r.json()) as CloudSyncResult;
        if (j.phase === 'done') return j;
      }
      throw new Error('cloud sync timed out (5 minutes)');
    },
    onSuccess: (result) => {
      // Only refresh the local list if the cloud actually returned
      // something — a CF block leaves it untouched.
      if (result.status === 'ok') qc.invalidateQueries({ queryKey: LIST_KEY });
    },
  });
}

// Push a single local user filament up to the cloud (creates a new
// cloud preset and, if this preset was already in the cloud, deletes
// the old one — the cloud doesn't support PUT/PATCH, see the API doc).
// Soft-fails on WAF block the same way as cloud-sync.
export interface CloudPushResult {
  status: 'ok' | 'rejected' | 'unreachable';
  cloud_setting_id?: string;
  diagnostics?: CloudDiagnostics;
}
export function useCloudPushFilament() {
  const qc = useQueryClient();
  return useMutation<CloudPushResult, Error, string>({
    mutationFn: async (id) => {
      const r = await fetch(
        `/api/user-filaments/${encodeURIComponent(id)}/cloud-push`,
        { method: 'POST' },
      );
      const j = await r.json();
      if (!r.ok) throw new Error(j?.error || `HTTP ${r.status}`);
      return j as CloudPushResult;
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: LIST_KEY }),
  });
}
