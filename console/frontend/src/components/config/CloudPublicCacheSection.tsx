import { useRef } from 'react';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { Library, RefreshCw, Trash2, AlertTriangle, CheckCircle2, Upload } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { uploadWithAuth } from '@spoolhard/ui/utils/uploadWithAuth';

// On-device cache of Bambu Cloud's public filament catalog (a slim
// {name, setting_id} JSONL on SD). Bambu's edge unreliably filters the
// public catalog out of the device's request, so we cache the working
// response the first time we get one and use it for all subsequent
// `inherits` lookups in the cloud-detail panel. This section lets the
// user see the cache state and force a refresh.
interface CacheStatus {
  present:  boolean;
  bytes?:   number;
  mtime_s?: number;
  entries?: number;
  path:     string;
}

interface RefreshResult {
  status: 'ok' | 'empty' | 'unreachable' | 'rejected' | 'io_error';
  entries?: number;
  error?:   string;
  preserved_existing_cache?: boolean;
}

function fmtTimestamp(epoch: number): string {
  if (!epoch) return '—';
  const d = new Date(epoch * 1000);
  return d.toLocaleString();
}

function fmtBytes(b: number): string {
  if (b < 1024) return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
  return `${(b / (1024 * 1024)).toFixed(2)} MB`;
}

export function CloudPublicCacheSection() {
  const qc = useQueryClient();

  const status = useQuery<CacheStatus>({
    queryKey: ['bambu-cloud-public-cache'],
    queryFn: () => fetch('/api/bambu-cloud/public-cache').then((r) => r.json()),
    refetchInterval: 30_000,
  });

  const refresh = useMutation<RefreshResult, Error, void>({
    mutationFn: async () => {
      const r = await fetch('/api/bambu-cloud/public-cache/refresh', { method: 'POST' });
      const j = (await r.json()) as RefreshResult;
      if (!r.ok) throw new Error(j?.error || `HTTP ${r.status}`);
      return j;
    },
    onSettled: () => qc.invalidateQueries({ queryKey: ['bambu-cloud-public-cache'] }),
  });

  const remove = useMutation<{ ok: boolean; removed: boolean }, Error, void>({
    mutationFn: async () => {
      const r = await fetch('/api/bambu-cloud/public-cache', { method: 'DELETE' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return r.json();
    },
    onSettled: () => qc.invalidateQueries({ queryKey: ['bambu-cloud-public-cache'] }),
  });

  const upload = useMutation<void, Error, File>({
    mutationFn: (f) => uploadWithAuth('/api/bambu-cloud/public-cache/upload', f, () => {}),
    onSettled: () => qc.invalidateQueries({ queryKey: ['bambu-cloud-public-cache'] }),
  });
  const fileInput = useRef<HTMLInputElement>(null);

  const s = status.data;

  return (
    <SectionCard
      title="Public catalog cache"
      icon={<Library size={16} />}
      description="Cached slim view of Bambu's public filament catalog (~85 KB JSONL on SD). Used to resolve the parent name shown next to a cloud-synced custom filament's `inherits` field. Bambu's edge unreliably filters the public catalog from the device's request, so once we get a working response we keep it; refresh manually when you want fresher data."
    >
      {status.isLoading && <div className="text-sm text-text-muted">Loading…</div>}

      {s && !s.present && (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-sm text-amber-300">
          <div className="flex items-center gap-2 font-medium">
            <AlertTriangle size={14} /> No cache yet
          </div>
          <div className="mt-1 text-xs text-amber-200/80">
            The first "Try fetching parent from cloud" attempt on a custom filament will
            populate this cache from {' '}
            <span className="font-mono">/v1/iot-service/api/slicer/setting?public=true</span>.
            Or click <strong>Refresh now</strong> to populate it pre-emptively.
          </div>
        </div>
      )}

      {s && s.present && (
        <div className="rounded-md border border-teal-500/30 bg-teal-500/10 p-3 text-sm text-teal-300">
          <div className="flex items-center gap-2 font-medium">
            <CheckCircle2 size={14} /> {s.entries ?? '—'} entries cached
          </div>
          <div className="mt-1 text-xs text-teal-200/80 grid grid-cols-1 md:grid-cols-3 gap-2 font-mono">
            <div>size: {s.bytes != null ? fmtBytes(s.bytes) : '—'}</div>
            <div>updated: {s.mtime_s ? fmtTimestamp(s.mtime_s) : '—'}</div>
            <div>path: {s.path}</div>
          </div>
        </div>
      )}

      {/* Refresh outcome surface — covers all four non-OK paths plus a
          success bubble for the user-confirmation case. */}
      {refresh.data && refresh.data.status === 'ok' && (
        <div className="mt-3 rounded-md border border-teal-500/30 bg-teal-500/10 p-2 text-xs text-teal-300">
          Refreshed — {refresh.data.entries} entries written.
        </div>
      )}
      {refresh.data && refresh.data.status === 'empty' && (
        <div className="mt-3 rounded-md border border-amber-500/30 bg-amber-500/10 p-2 text-xs text-amber-300 space-y-1">
          <div>
            Bambu's cloud returned an empty public catalog — typical when their edge
            filters the device's request signature.
          </div>
          {refresh.data.preserved_existing_cache && (
            <div className="text-amber-200/70">
              Existing cache preserved (we never overwrite a working catalog with an
              empty response).
            </div>
          )}
          <div className="opacity-80">Try again in a few minutes; it works intermittently.</div>
        </div>
      )}
      {refresh.data && (refresh.data.status === 'unreachable' || refresh.data.status === 'rejected' || refresh.data.status === 'io_error') && (
        <div className="mt-3 rounded-md border border-red-500/30 bg-red-500/10 p-2 text-xs text-red-300">
          Refresh failed ({refresh.data.status}): {refresh.data.error || 'unknown error'}
        </div>
      )}
      {refresh.error && (
        <div className="mt-3 rounded-md border border-red-500/30 bg-red-500/10 p-2 text-xs text-red-300">
          {(refresh.error as Error).message}
        </div>
      )}

      <div className="flex items-center gap-2 mt-3 flex-wrap">
        <Button onClick={() => refresh.mutate()} disabled={refresh.isPending}>
          <RefreshCw size={14} className={`inline mr-1.5 ${refresh.isPending ? 'animate-spin' : ''}`} />
          {refresh.isPending ? 'Fetching from cloud…' : (s?.present ? 'Refresh now' : 'Fetch catalog')}
        </Button>
        <Button variant="secondary" onClick={() => fileInput.current?.click()} disabled={upload.isPending}>
          <Upload size={14} className="inline mr-1.5" />
          {upload.isPending ? 'Uploading…' : 'Upload JSONL'}
        </Button>
        <input
          ref={fileInput}
          type="file"
          accept=".jsonl,application/x-ndjson,application/json,text/plain"
          className="hidden"
          onChange={(e) => {
            const f = e.target.files?.[0];
            if (f) upload.mutate(f);
            if (fileInput.current) fileInput.current.value = '';
          }}
        />
        {s?.present && (
          <Button variant="secondary" onClick={() => remove.mutate()} disabled={remove.isPending}>
            <Trash2 size={14} className="inline mr-1.5" />
            Clear cache
          </Button>
        )}
      </div>

      {upload.error && (
        <div className="mt-3 rounded-md border border-red-500/30 bg-red-500/10 p-2 text-xs text-red-300">
          Upload failed: {(upload.error as Error).message}
        </div>
      )}

      {/* Hint for the desktop-side dump command, since the cloud-fetch
          path is unreliable from the device. */}
      <details className="mt-3 text-[11px] text-text-muted">
        <summary className="cursor-pointer">Generate the JSONL from a desktop</summary>
        <pre className="mt-2 p-2 bg-surface-body/40 border border-surface-border rounded-md overflow-auto whitespace-pre-wrap break-all">
{`# Replace $TOKEN with your Bambu Cloud token (same paste-blob you used in
# Config > BambuLab Cloud, decoded). Produces a 1600-line JSONL on stdout.

curl -s -H "Authorization: Bearer $TOKEN" \\
  "https://api.bambulab.com/v1/iot-service/api/slicer/setting?version=02.04.00.70&public=true" \\
| python3 -c "import sys, json
d = json.load(sys.stdin)
for r in d['filament']['public']:
    if r.get('name') and r.get('setting_id'):
        print(json.dumps({'name': r['name'], 'setting_id': r['setting_id']}, separators=(',',':')))" \\
> cloud_filaments_pub.jsonl`}
        </pre>
      </details>
    </SectionCard>
  );
}
