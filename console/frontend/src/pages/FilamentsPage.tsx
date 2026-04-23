import { useEffect, useState } from 'react';
import {
  FlaskConical, Library, User as UserIcon,
  Plus, Trash2, ChevronDown, ChevronRight, Cloud, CloudUpload, RefreshCw, Thermometer, Gauge,
} from 'lucide-react';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';
import { SubTabBar, type SubTab } from '@spoolhard/ui/components/SubTabBar';
import {
  useUserFilaments, useDeleteUserFilament,
  useCloudSyncFilaments, useCloudPushFilament,
  type UserFilament,
} from '../hooks/useUserFilaments';
import { useFilamentsDb, type FilamentEntry } from '../hooks/useFilamentsDb';
import { useBambuCloud } from '../hooks/useBambuCloud';
import { UserFilamentForm } from '../components/filaments/UserFilamentForm';

type FilamentsTab = 'stock' | 'user';

const tabs: SubTab<FilamentsTab>[] = [
  { id: 'stock', label: 'Stock',  icon: <Library  size={14} /> },
  { id: 'user',  label: 'Custom', icon: <UserIcon size={14} /> },
];

function getInitialTab(): FilamentsTab {
  const params = new URLSearchParams(window.location.search);
  const t = params.get('tab');
  if (t === 'stock' || t === 'user') return t;
  return 'stock';
}

export function FilamentsPage() {
  const [activeTab, setActiveTab] = useState<FilamentsTab>(getInitialTab);
  const navigate = (t: FilamentsTab) => {
    const url = new URL(window.location.href);
    url.searchParams.set('tab', t);
    window.history.replaceState(null, '', url.toString());
    setActiveTab(t);
  };
  useEffect(() => {
    const onPop = () => setActiveTab(getInitialTab());
    window.addEventListener('popstate', onPop);
    return () => window.removeEventListener('popstate', onPop);
  }, []);

  return (
    <div className="space-y-4">
      <SubTabBar tabs={tabs} active={activeTab} onChange={navigate} />
      <div className="animate-in">
        {activeTab === 'stock' && <StockFilamentsSection />}
        {activeTab === 'user'  && <UserFilamentsSection />}
      </div>
    </div>
  );
}

// ── Stock tab — read-only view of /sd/filaments.jsonl ─────────
function StockFilamentsSection() {
  const { data: db, isLoading } = useFilamentsDb();
  const [search, setSearch] = useState('');
  const all = db?.present ? db.entries : [];
  const filtered = search
    ? all.filter((r) =>
        r.name.toLowerCase().includes(search.toLowerCase()) ||
        r.material.toLowerCase().includes(search.toLowerCase()) ||
        r.brand.toLowerCase().includes(search.toLowerCase()),
      )
    : all;

  return (
    <Card title={`Stock filaments${all.length ? ` (${all.length})` : ''}`}>
      {isLoading && <div className="text-sm text-text-muted">Loading filaments DB…</div>}
      {!isLoading && all.length === 0 && (
        <div className="text-sm text-text-muted py-8 text-center">
          No filaments DB on the device. Upload one in Config → Filaments library.
        </div>
      )}
      {all.length > 0 && (
        <>
          <input
            type="text"
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            placeholder="Search by name, material, or vendor…"
            className="w-full mb-3 rounded-md border border-surface-border bg-surface-input px-3 py-2 text-sm text-text-primary"
          />
          <div className="space-y-1 max-h-[60vh] overflow-auto">
            {filtered.slice(0, 200).map((r) => (
              <StockFilamentRow key={r.setting_id ?? r.name} entry={r} />
            ))}
            {filtered.length > 200 && (
              <div className="text-xs text-text-muted text-center py-2">
                {filtered.length - 200} more — refine search to narrow.
              </div>
            )}
          </div>
        </>
      )}
    </Card>
  );
}

function StockFilamentRow({ entry }: { entry: FilamentEntry }) {
  const [open, setOpen] = useState(false);
  // Stock entries don't carry per-nozzle PA arrays today (the bambu-
  // filaments build pipeline only emits the resolved scalar) so the
  // detail block shows a single PA value when present.
  return (
    <div className="rounded-md bg-surface-input">
      <div
        className="flex items-center gap-3 px-3 py-2 text-sm cursor-pointer hover:bg-surface-card-hover"
        onClick={() => setOpen((v) => !v)}
        role="button"
        aria-expanded={open}
      >
        <span className="flex-shrink-0 text-text-muted">
          {open ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
        </span>
        <FlaskConical size={12} className="text-text-muted flex-shrink-0" />
        <div className="flex-1 min-w-0 truncate text-text-primary">{entry.name}</div>
        <div className="text-xs text-text-muted">
          {entry.material}{entry.subtype ? ` · ${entry.subtype}` : ''}
        </div>
        <div className="text-xs text-text-muted">{entry.brand}</div>
      </div>
      {open && (
        <div className="border-t border-surface-border px-3 py-3 text-xs space-y-2 bg-surface-body/30">
          <div className="grid grid-cols-2 md:grid-cols-3 gap-x-4 gap-y-2 font-mono">
            <DetailField label="Setting ID"   value={entry.setting_id || '—'} />
            <DetailField label="Base ID"      value={entry.base_id    || '—'} />
            <DetailField label="Filament ID"  value={entry.filament_id || '—'} mono />
            <DetailField label="Vendor"       value={entry.brand      || '—'} />
            <DetailField label="Material"     value={entry.material   || '—'} />
            <DetailField label="Subtype"      value={entry.subtype    || '—'} />
            <DetailField
              label="Nozzle temp"
              icon={<Thermometer size={10} />}
              value={
                typeof entry.nozzle_temp_min === 'number' && typeof entry.nozzle_temp_max === 'number'
                  ? `${entry.nozzle_temp_min}–${entry.nozzle_temp_max} °C`
                  : '—'
              }
            />
            <DetailField
              label="Density"
              value={typeof entry.density === 'number' && entry.density > 0 ? `${entry.density} g/cm³` : '—'}
            />
            <DetailField
              label="Pressure advance (default)"
              icon={<Gauge size={10} />}
              value={
                typeof entry.pressure_advance === 'number' && entry.pressure_advance > 0
                  ? entry.pressure_advance.toFixed(3)
                  : '—'
              }
            />
          </div>
        </div>
      )}
    </div>
  );
}

function DetailField({ label, value, icon, mono }: { label: string; value: string; icon?: React.ReactNode; mono?: boolean }) {
  return (
    <div>
      <div className="flex items-center gap-1 text-text-muted">
        {icon}{label}
      </div>
      <div className={`${mono ? 'font-mono ' : ''}text-text-primary truncate`}>{value}</div>
    </div>
  );
}

// ── User tab — local CRUD + cloud sync/push ───────────────────
function UserFilamentsSection() {
  const { data, isLoading } = useUserFilaments();
  const cloud = useBambuCloud();
  const sync = useCloudSyncFilaments();
  const del = useDeleteUserFilament();
  const [creating, setCreating] = useState(false);
  const [editing, setEditing]   = useState<UserFilament | null>(null);
  const rows = data?.rows ?? [];

  const hasToken = !!cloud.data?.configured;

  const actions = (
    <>
      {hasToken && (
        <Button
          variant="secondary"
          onClick={() => sync.mutate()}
          disabled={sync.isPending}
        >
          <RefreshCw size={14} className="inline mr-1" />
          {sync.isPending ? 'Syncing…' : 'Sync from cloud'}
        </Button>
      )}
      <Button onClick={() => { setEditing(null); setCreating(true); }}>
        <Plus size={14} className="inline mr-1" />
        New
      </Button>
    </>
  );

  return (
    <Card title={`Custom filaments (${rows.length})`} actions={actions}>
      {sync.isError && (
        <div className="mb-3 rounded-md border border-red-500/30 bg-red-500/10 p-3 text-sm text-red-300">
          <div className="font-medium">Sync request failed</div>
          <div className="mt-1 text-xs font-mono break-all">
            {(sync.error as Error)?.message || 'Unknown error'}
          </div>
        </div>
      )}
      {sync.data && sync.data.status !== 'ok' && (
        <div className={`mb-3 rounded-md border p-3 text-sm space-y-2 ${
          sync.data.status === 'unreachable'
            ? 'border-amber-500/30 bg-amber-500/10 text-amber-300'
            : 'border-red-500/30 bg-red-500/10 text-red-300'
        }`}>
          <div>
            {sync.data.status === 'unreachable'
              ? 'Couldn\'t reach Bambu Cloud to sync (the device\'s network is being blocked by Cloudflare). Local list is unchanged.'
              : 'Cloud sync rejected by Bambu — your token may be invalid or the request shape changed.'}
          </div>
          {sync.data.diagnostics && (
            <details className="text-xs font-mono">
              <summary className="cursor-pointer">Show details</summary>
              <div className="mt-2 space-y-1 break-all">
                <div>stage: {sync.data.diagnostics.stage}</div>
                <div>{sync.data.diagnostics.http_status} {sync.data.diagnostics.cf_blocked ? '[CF block]' : ''}</div>
                <div className="opacity-80">{sync.data.diagnostics.request_url}</div>
                {sync.data.diagnostics.response_body && (
                  <pre className="mt-1 max-h-40 overflow-auto bg-surface-body/40 p-2 rounded whitespace-pre-wrap">
                    {sync.data.diagnostics.response_body}
                  </pre>
                )}
              </div>
            </details>
          )}
        </div>
      )}
      {sync.data?.status === 'ok' && ((sync.data.added ?? 0) > 0 || (sync.data.updated ?? 0) > 0) && (
        <div className="mb-3 rounded-md border border-teal-500/30 bg-teal-500/10 p-3 text-sm text-teal-300">
          Synced — added {sync.data.added ?? 0}, updated {sync.data.updated ?? 0}.
        </div>
      )}

      {(creating || editing) && (
        <div className="mb-3">
          <UserFilamentForm
            initial={editing ?? undefined}
            onClose={() => { setCreating(false); setEditing(null); }}
          />
        </div>
      )}

      {isLoading && <div className="text-sm text-text-muted">Loading…</div>}
      {!isLoading && rows.length === 0 && (
        <div className="text-sm text-text-muted py-8 text-center">
          No custom filaments yet — click <strong>New</strong> to create one.
          {hasToken && <> Or <strong>Sync from cloud</strong> to import your Bambu Cloud presets.</>}
        </div>
      )}
      <div className="space-y-2">
        {rows.map((r) => (
          <UserFilamentRow
            key={r.setting_id}
            r={r}
            hasToken={hasToken}
            onEdit={() => { setCreating(false); setEditing(r); }}
            onDelete={() => del.mutate(r.setting_id)}
          />
        ))}
      </div>
    </Card>
  );
}

function UserFilamentRow({
  r, hasToken, onEdit, onDelete,
}: {
  r: UserFilament;
  hasToken: boolean;
  onEdit: () => void;
  onDelete: () => void;
}) {
  const [open, setOpen] = useState(false);
  const push = useCloudPushFilament();

  // Sync state pill: matches updated_at vs cloud_synced_at to detect
  // local edits that haven't been pushed.
  let cloudPill: { text: string; cls: string } | null = null;
  if (r.cloud_setting_id) {
    if (r.updated_at > r.cloud_synced_at + 1) {
      cloudPill = { text: 'out of sync', cls: 'bg-amber-500/15 text-amber-300 border-amber-500/30' };
    } else {
      cloudPill = { text: 'in cloud',    cls: 'bg-teal-500/15 text-teal-300 border-teal-500/30' };
    }
  } else {
    cloudPill = { text: 'local only',   cls: 'bg-surface-input text-text-muted border-surface-border' };
  }

  return (
    <div className="rounded-md border border-surface-border bg-surface-input">
      <div
        className="flex items-center gap-3 p-3 cursor-pointer hover:bg-surface-card-hover"
        onClick={() => setOpen((v) => !v)}
        role="button"
      >
        <span className="flex-shrink-0 text-text-muted">
          {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        </span>
        <FlaskConical size={14} className="text-text-muted" />
        <div className="flex-1 min-w-0">
          <div className="text-sm font-medium text-text-primary truncate">
            {r.name || '(unnamed)'}
          </div>
          <div className="text-xs text-text-muted truncate">
            {r.filament_type} · {r.filament_vendor || '—'}
            {r.nozzle_temp_max > 0 && ` · ${r.nozzle_temp_max}°C`}
          </div>
        </div>
        <span className={`text-xs px-2 py-0.5 rounded-full border ${cloudPill.cls}`}>
          {cloudPill.text}
        </span>
        <button
          onClick={(e) => { e.stopPropagation(); onDelete(); }}
          className="text-text-muted hover:text-red-400 transition-colors cursor-pointer"
          aria-label="Delete filament"
        >
          <Trash2 size={16} />
        </button>
      </div>
      {open && (
        <div className="border-t border-surface-border p-3 space-y-3 text-sm">
          <div className="grid grid-cols-2 gap-3 font-mono text-xs">
            <div>
              <div className="text-text-muted">Setting ID</div>
              <div className="text-text-primary truncate">{r.setting_id}</div>
            </div>
            <div>
              <div className="text-text-muted">Cloud ID</div>
              <div className="text-text-primary truncate">{r.cloud_setting_id || '—'}</div>
            </div>
            <div>
              <div className="text-text-muted">Base ID</div>
              <div className="text-text-primary">{r.base_id || '—'}</div>
            </div>
            <div>
              <div className="text-text-muted">Density</div>
              <div className="text-text-primary">{r.density > 0 ? `${r.density} g/cm³` : '—'}</div>
            </div>
            <div>
              <div className="text-text-muted">PA default (K)</div>
              <div className="text-text-primary">{r.pressure_advance > 0 ? r.pressure_advance.toFixed(3) : '—'}</div>
            </div>
            <div>
              <div className="text-text-muted">PA per nozzle</div>
              <div className="text-text-primary">
                {r.pa_by_nozzle && r.pa_by_nozzle.length > 0
                  ? r.pa_by_nozzle.map((e) => `${e.nozzle}mm:${e.k.toFixed(3)}`).join(', ')
                  : '—'}
              </div>
            </div>
          </div>
          <div className="flex flex-wrap gap-2">
            <Button variant="secondary" onClick={onEdit}>Edit</Button>
            {hasToken && (
              <Button
                variant="secondary"
                onClick={() => push.mutate(r.setting_id)}
                disabled={push.isPending}
              >
                {r.cloud_setting_id ? <CloudUpload size={14} className="inline mr-1" />
                                    : <Cloud       size={14} className="inline mr-1" />}
                {push.isPending ? 'Pushing…' : (r.cloud_setting_id ? 'Push update' : 'Push to cloud')}
              </Button>
            )}
          </div>
          {push.isError && (
            <div className="rounded-md border border-red-500/30 bg-red-500/10 p-2 text-xs text-red-300 break-all font-mono">
              {(push.error as Error)?.message || 'Push request failed'}
            </div>
          )}
          {push.data && push.data.status !== 'ok' && (
            <div className={`rounded-md border p-2 text-xs space-y-2 ${
              push.data.status === 'unreachable'
                ? 'border-amber-500/30 bg-amber-500/10 text-amber-300'
                : 'border-red-500/30 bg-red-500/10 text-red-300'
            }`}>
              <div>
                {push.data.status === 'unreachable'
                  ? 'Couldn\'t reach Bambu Cloud — preset stays local.'
                  : push.data.diagnostics?.http_status === 401 ||
                    push.data.diagnostics?.http_status === 403
                    ? 'Push rejected — re-check your Bambu Cloud token.'
                    : 'Push rejected by Bambu Cloud — see details.'}
              </div>
              {push.data.diagnostics && (
                <details className="font-mono">
                  <summary className="cursor-pointer">Show details</summary>
                  <div className="mt-1 space-y-1 break-all">
                    <div>stage: {push.data.diagnostics.stage}</div>
                    <div>{push.data.diagnostics.http_status} {push.data.diagnostics.cf_blocked ? '[CF block]' : ''}</div>
                    <div className="opacity-80">{push.data.diagnostics.request_url}</div>
                    {push.data.diagnostics.response_body && (
                      <pre className="mt-1 max-h-40 overflow-auto bg-surface-body/40 p-2 rounded whitespace-pre-wrap">
                        {push.data.diagnostics.response_body}
                      </pre>
                    )}
                  </div>
                </details>
              )}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
