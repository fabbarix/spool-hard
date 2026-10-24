import { useEffect, useMemo, useState } from 'react';
import { Trash2, ChevronDown, ChevronRight, Gauge, BatteryLow, Search } from 'lucide-react';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';
import { ConfirmDialog } from '@spoolhard/ui/components/ConfirmDialog';
import { ErrorBanner } from '@spoolhard/ui/components/ErrorBanner';
import { SkeletonRows } from '@spoolhard/ui/components/Skeleton';
import { useSpools, useSpoolDelete, useSpoolUpsert, type SpoolRecord } from '../hooks/useSpools';
import { SpoolDetailPanel } from '../components/spools/SpoolDetailPanel';

// Same (offset, limit, material) as the dashboard's WS-pushed entry, so the
// list shares its cache and stays live. Search/filter run client-side over
// the whole set; Prev/Next only appear past this many spools.
const PAGE_SIZE = 200;
const LOW_GRAMS = 100;
const LOW_PCT = 10;
const UNDO_MS = 8000;

type Filter = 'active' | 'low' | 'empty' | 'all';

// weight_current is the net filament weight (main.cpp stamps scale net),
// minus what prints have consumed since that weighing.
function remainingGrams(r: SpoolRecord): number | null {
  if (typeof r.weight_current !== 'number' || r.weight_current < 0) return null;
  const used = typeof r.consumed_since_weight === 'number' ? r.consumed_since_weight : 0;
  return Math.max(0, Math.round(r.weight_current - used));
}

// Full-spool filament weight, or null when unknown (no bar is drawn then).
function fullGrams(r: SpoolRecord): number | null {
  if (typeof r.weight_advertised === 'number' && r.weight_advertised > 0) return r.weight_advertised;
  if (typeof r.weight_new === 'number' && r.weight_new > 0) {
    const core = typeof r.weight_core === 'number' && r.weight_core > 0 ? r.weight_core : 0;
    return Math.max(1, r.weight_new - core);
  }
  return null;
}

function percentLeft(r: SpoolRecord): number | null {
  const g = remainingGrams(r), full = fullGrams(r);
  if (g === null || full === null) return null;
  return Math.max(0, Math.min(100, Math.round((g / full) * 100)));
}

function isLow(r: SpoolRecord): boolean {
  if (r.is_empty) return false;
  const g = remainingGrams(r), pct = percentLeft(r);
  if (g === null) return false;
  return g <= LOW_GRAMS || (pct !== null && pct <= LOW_PCT);
}

function spoolTitle(r: SpoolRecord): string {
  return `${r.brand || 'Unknown'} · ${r.material_type || '—'}`
    + (r.material_subtype ? ` (${r.material_subtype})` : '')
    + (r.color_name ? ` · ${r.color_name}` : '');
}

function matches(r: SpoolRecord, q: string): boolean {
  if (!q) return true;
  const hay = [r.brand, r.material_type, r.material_subtype, r.color_name, r.tag_id, r.id, r.slicer_filament]
    .filter(Boolean).join(' ').toLowerCase();
  return q.toLowerCase().split(/\s+/).every((t) => hay.includes(t));
}

// SpoolsPage used to wrap a SubTabBar with [Spools | Core weights]; the
// Core weights view was promoted to a top-level "Empty weights" tab in
// App.tsx, leaving Spools as a single section. The wrapper is now a
// thin pass-through to SpoolsListSection.
export function SpoolsPage() {
  return (
    <div className="space-y-4 animate-in">
      <SpoolsListSection />
    </div>
  );
}

function SpoolsListSection() {
  const [offset, setOffset] = useState(0);
  const [filter, setFilter] = useState<Filter>('active');
  const [query, setQuery] = useState('');
  const { data, isLoading, isError, error, refetch, isFetching } = useSpools(offset, PAGE_SIZE, '');
  const del = useSpoolDelete();
  const restore = useSpoolUpsert();
  const [pendingDelete, setPendingDelete] = useState<SpoolRecord | null>(null);
  const [undo, setUndo] = useState<SpoolRecord | null>(null);

  useEffect(() => {
    if (!undo) return;
    const t = setTimeout(() => setUndo(null), UNDO_MS);
    return () => clearTimeout(t);
  }, [undo]);

  const all = useMemo(() => data?.rows ?? [], [data]);
  const total = data?.total ?? 0;
  const counts = useMemo(() => ({
    active: all.filter((r) => !r.is_empty).length,
    low: all.filter(isLow).length,
    empty: all.filter((r) => r.is_empty).length,
    all: all.length,
  }), [all]);

  const rows = useMemo(() => all.filter((r) => {
    if (filter === 'active' && r.is_empty) return false;
    if (filter === 'low' && !isLow(r)) return false;
    if (filter === 'empty' && !r.is_empty) return false;
    return matches(r, query.trim());
  }), [all, filter, query]);

  const chips: { id: Filter; label: string }[] = [
    { id: 'active', label: 'Active' },
    { id: 'low', label: 'Running low' },
    { id: 'empty', label: 'Empty' },
    { id: 'all', label: 'All' },
  ];

  const paging = total > PAGE_SIZE && (
    <div className="flex items-center gap-2">
      <Button variant="secondary" onClick={() => setOffset(Math.max(0, offset - PAGE_SIZE))} disabled={offset === 0}>Prev</Button>
      <Button variant="secondary" onClick={() => setOffset(offset + PAGE_SIZE)} disabled={offset + PAGE_SIZE >= total}>Next</Button>
    </div>
  );

  return (
    <Card title={`Spools (${total})`} actions={paging || undefined}>
      <div className="space-y-3">
        <label className="flex items-center gap-2 rounded-button border border-surface-border bg-surface-input px-3 min-h-11 focus-within:border-brand-500/60">
          <Search size={14} className="text-text-muted shrink-0" aria-hidden="true" />
          <input
            type="search"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Search brand, material, colour, tag"
            aria-label="Search spools"
            className="min-w-0 flex-1 bg-transparent text-sm text-text-primary placeholder:text-text-muted outline-none"
          />
        </label>

        <div role="group" aria-label="Filter spools" className="flex flex-wrap gap-2">
          {chips.map((c) => (
            <button
              key={c.id}
              type="button"
              aria-pressed={filter === c.id}
              onClick={() => setFilter(c.id)}
              className={`min-h-11 rounded-full border px-4 text-sm font-medium cursor-pointer transition-colors ${
                filter === c.id
                  ? 'border-brand-500 bg-brand-500 text-surface-body'
                  : 'border-surface-border bg-surface-input text-text-secondary hover:text-text-primary'
              }`}
            >
              {c.label} <span className="font-mono tabular-nums opacity-80">({counts[c.id]})</span>
            </button>
          ))}
        </div>

        {isError && (
          <ErrorBanner
            title="Could not load spools"
            detail={error instanceof Error ? error.message : undefined}
            onRetry={() => refetch()}
            retrying={isFetching}
          />
        )}
        {isLoading && <SkeletonRows rows={4} />}

        {!isLoading && !isError && rows.length === 0 && (
          <div className="py-8 text-center space-y-1">
            {all.length === 0 ? (
              <>
                <div className="text-base font-semibold text-text-primary">No spools yet</div>
                <div className="text-sm text-text-muted">Scan a SpoolHard-tagged spool to add your first one.</div>
              </>
            ) : (
              <>
                <div className="text-base font-semibold text-text-primary">No spools match</div>
                <div className="text-sm text-text-muted">
                  {query ? `Nothing matches "${query.trim()}" in this filter.` : 'Nothing in this filter.'}
                </div>
                {(query || filter !== 'all') && (
                  <div className="pt-2">
                    <Button variant="secondary" onClick={() => { setQuery(''); setFilter('all'); }}>Show all spools</Button>
                  </div>
                )}
              </>
            )}
          </div>
        )}

        <div className="space-y-2">
          {rows.map((r) => <SpoolRow key={r.id} r={r} onDelete={() => setPendingDelete(r)} />)}
        </div>
      </div>

      <ConfirmDialog
        open={pendingDelete !== null}
        title="Delete spool?"
        message={pendingDelete && (
          <>
            {pendingDelete.brand || 'Unknown'} · {pendingDelete.material_type || '—'}
            {pendingDelete.color_name && ` · ${pendingDelete.color_name}`}
            {' '}(<span className="font-mono">{pendingDelete.tag_id}</span>) will be removed from the
            spool database. You can undo for a few seconds afterwards.
          </>
        )}
        confirmLabel={del.isPending ? 'Deleting...' : 'Delete'}
        busy={del.isPending}
        onConfirm={() => {
          if (!pendingDelete) return;
          const rec = pendingDelete;
          del.mutate(rec.id, {
            onSuccess: () => setUndo(rec),
            onSettled: () => setPendingDelete(null),
          });
        }}
        onCancel={() => setPendingDelete(null)}
      />

      {undo && (
        <div
          role="status"
          className="fixed bottom-4 left-1/2 z-40 flex w-[calc(100%-2rem)] max-w-md -translate-x-1/2 items-center gap-3 rounded-card border border-surface-border bg-surface-card px-4 py-2 shadow-xl"
        >
          <span className="min-w-0 flex-1 truncate text-sm text-text-primary">
            {restore.isError ? 'Restore failed' : `${spoolTitle(undo)} deleted`}
          </span>
          <Button
            variant="primary"
            disabled={restore.isPending}
            onClick={() => restore.mutate(undo, { onSuccess: () => setUndo(null) })}
          >
            {restore.isPending ? 'Restoring…' : 'Undo'}
          </Button>
        </div>
      )}
    </Card>
  );
}

function WeightMeter({ r }: { r: SpoolRecord }) {
  const g = remainingGrams(r);
  if (g === null) return null;
  const pct = percentLeft(r);
  const low = isLow(r);
  return (
    <div className="flex-shrink-0 w-24 sm:w-44 text-right">
      <div className={`text-sm font-mono tabular-nums ${low ? 'text-status-error' : 'text-brand-400'}`}>{g}g</div>
      {pct !== null && (
        <>
          <div
            className="mt-1 h-2 rounded-full bg-surface-border overflow-hidden"
            role="progressbar"
            aria-valuemin={0}
            aria-valuemax={100}
            aria-valuenow={pct}
            aria-label="Filament remaining"
          >
            <div className={`h-full ${low ? 'bg-status-error' : 'bg-brand-500'}`} style={{ width: `${pct}%` }} />
          </div>
          <div className="text-[11px] text-text-muted">{pct}% left</div>
        </>
      )}
    </div>
  );
}

function SpoolRow({ r, onDelete }: { r: SpoolRecord; onDelete: () => void }) {
  const [open, setOpen] = useState(false);
  const kValues = r.ext?.k_values ?? [];

  return (
    <div className="rounded-md border border-surface-border bg-surface-input">
      {/* Click the whole header row (minus the delete button) to toggle. The
          chevron is now a pure affordance, not the only interactive target. */}
      <div
        className="flex items-center gap-3 p-3 cursor-pointer hover:bg-surface-card-hover"
        onClick={() => setOpen((v) => !v)}
        role="button"
        tabIndex={0}
        aria-expanded={open}
        onKeyDown={(e) => {
          if (e.target === e.currentTarget && (e.key === 'Enter' || e.key === ' ')) {
            e.preventDefault();
            setOpen((v) => !v);
          }
        }}
      >
        <span className="flex-shrink-0 text-text-muted">
          {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        </span>
        <div
          className="w-3 h-3 rounded-full flex-shrink-0"
          style={{ background: r.color_code ? `#${r.color_code}` : '#444' }}
        />
        <div className="flex-1 min-w-0">
          <div className={`text-sm font-medium truncate flex items-center gap-2 ${r.is_empty ? 'text-text-muted' : 'text-text-primary'}`}>
            {r.brand || 'Unknown'} · {r.material_type || '—'}
            {r.material_subtype && ` (${r.material_subtype})`}
            {r.color_name && ` · ${r.color_name}`}
            {r.is_empty && (
              <span className="inline-flex items-center gap-1 text-[10px] uppercase tracking-wider text-amber-400 bg-amber-500/10 border border-amber-500/30 rounded px-1.5 py-0.5">
                <BatteryLow size={10} /> Empty
              </span>
            )}
            {kValues.length > 0 && (
              <span className="inline-flex items-center gap-1 text-[10px] uppercase tracking-wider text-brand-400">
                <Gauge size={10} /> K ×{kValues.length}
              </span>
            )}
          </div>
          <div className="text-xs text-text-muted font-mono truncate">
            {r.tag_id} · {r.data_origin || 'Unknown'}
            {r.slicer_filament && <> · <span title="AMS tray_info_idx — auto-synced from the printer">idx:{r.slicer_filament}</span></>}
          </div>
        </div>
        <WeightMeter r={r} />
        <button
          onClick={(e) => { e.stopPropagation(); onDelete(); }}
          className="-m-2 p-2 text-text-muted hover:text-red-400 transition-colors cursor-pointer"
          aria-label={`Delete spool ${r.brand || 'Unknown'} ${r.material_type || ''} ${r.tag_id}`}
          title="Delete spool"
        >
          <Trash2 size={16} />
        </button>
      </div>

      {open && <SpoolDetailPanel spool={r} />}
    </div>
  );
}
