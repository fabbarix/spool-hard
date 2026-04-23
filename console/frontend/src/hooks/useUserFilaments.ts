import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { FilamentEntry } from './useFilamentsDb';

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
  // Local-stock linkage: when the user picks a stock filament as the
  // base via the form's "Base on a stock filament" picker, this holds
  // that stock entry's setting_id (e.g. "Bambu PETG Basic @base").
  // Empty fields on the custom inherit from this parent at display
  // time. Empty for cloud-synced customs (their `base_id` is in
  // Bambu's GFXX00 namespace, which doesn't map to a local entry).
  parent_setting_id?: string;
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

// What each field on a custom filament resolves to after merging the
// parent's values for any field the custom hasn't explicitly set. The
// `*Inherited` flags tell the UI which fields came from the parent vs.
// were typed by the user — so the form can label overrides clearly.
export interface ResolvedUserFilament {
  filament_type:    string;
  filament_subtype: string;
  filament_vendor:  string;
  filament_id:      string;
  nozzle_temp_min:  number;          // -1 = parent had no value either
  nozzle_temp_max:  number;
  density:          number;          // 0 = unset everywhere
  pressure_advance: number;
  pa_by_nozzle:     PaByNozzleEntry[];
  parent_name:      string | null;   // for "(inherits from <name>)" copy
  inherited: {
    filament_type:    boolean;
    filament_subtype: boolean;
    filament_vendor:  boolean;
    filament_id:      boolean;
    nozzle_temp_min:  boolean;
    nozzle_temp_max:  boolean;
    density:          boolean;
    pressure_advance: boolean;
    pa_by_nozzle:     boolean;
  };
}

// Sentinel detection — the firmware encodes "unset" as -1 for ints,
// 0 for floats, "" for strings, [] for the PA-per-nozzle list. Centralised
// here so the form and the display rows agree on what counts as "not set".
const isUnsetInt   = (v: number | undefined) => v === undefined || v < 0;
const isUnsetFloat = (v: number | undefined) => v === undefined || v <= 0;
const isUnsetStr   = (v: string | undefined) => !v || v.length === 0;
const isUnsetPaList = (v: PaByNozzleEntry[] | undefined) => !v || v.length === 0;

// Walk the parent chain (one level today — local stock is flat) and
// return a fully-merged view. `parent` is looked up by the caller from
// useFilamentsDb (the stock library); we keep this function pure so it
// can be called from anywhere without grabbing the React Query cache
// directly.
export function resolveUserFilament(
  custom: UserFilament,
  parent: FilamentEntry | null,
): ResolvedUserFilament {
  const inh = {
    filament_type:    isUnsetStr(custom.filament_type)    && !!parent?.material,
    filament_subtype: isUnsetStr(custom.filament_subtype) && !!parent?.subtype,
    filament_vendor:  isUnsetStr(custom.filament_vendor)  && !!parent?.brand,
    filament_id:      isUnsetStr(custom.filament_id)      && !!parent?.filament_id,
    nozzle_temp_min:  isUnsetInt(custom.nozzle_temp_min)  && typeof parent?.nozzle_temp_min === 'number',
    nozzle_temp_max:  isUnsetInt(custom.nozzle_temp_max)  && typeof parent?.nozzle_temp_max === 'number',
    density:          isUnsetFloat(custom.density)        && typeof parent?.density === 'number',
    pressure_advance: isUnsetFloat(custom.pressure_advance) && typeof parent?.pressure_advance === 'number',
    pa_by_nozzle:     isUnsetPaList(custom.pa_by_nozzle), // parents don't carry pa_by_nozzle today
  };
  return {
    filament_type:    inh.filament_type    ? (parent!.material ?? '')   : custom.filament_type,
    filament_subtype: inh.filament_subtype ? (parent!.subtype  ?? '')   : (custom.filament_subtype ?? ''),
    filament_vendor:  inh.filament_vendor  ? (parent!.brand    ?? '')   : custom.filament_vendor,
    filament_id:      inh.filament_id      ? (parent!.filament_id ?? '') : (custom.filament_id ?? ''),
    nozzle_temp_min:  inh.nozzle_temp_min  ? (parent!.nozzle_temp_min ?? -1) : custom.nozzle_temp_min,
    nozzle_temp_max:  inh.nozzle_temp_max  ? (parent!.nozzle_temp_max ?? -1) : custom.nozzle_temp_max,
    density:          inh.density          ? (parent!.density ?? 0)     : custom.density,
    pressure_advance: inh.pressure_advance ? (parent!.pressure_advance ?? 0) : custom.pressure_advance,
    pa_by_nozzle:     custom.pa_by_nozzle ?? [],
    parent_name:      parent?.name ?? null,
    inherited:        inh,
  };
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
