import { useMemo, useState } from 'react';
import { Trash2, ChevronDown, ChevronRight, Gauge, BatteryLow } from 'lucide-react';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';
import { useSpools, useSpoolDelete, type SpoolRecord } from '../hooks/useSpools';
import { SpoolDetailPanel } from '../components/spools/SpoolDetailPanel';

const PAGE_SIZE = 25;

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
  const [showEmpty, setShowEmpty] = useState(false);
  const { data, isLoading } = useSpools(offset, PAGE_SIZE);
  const del = useSpoolDelete();

  const rawRows = data?.rows ?? [];
  // Filter on the client because the backend has no "include empty" flag
  // — the page is paginated by the firmware though, so empty spools still
  // count toward the offset/total. Honest with the user: show the filtered
  // count and the underlying total side-by-side.
  const rows = useMemo(
    () => (showEmpty ? rawRows : rawRows.filter((r) => !r.is_empty)),
    [rawRows, showEmpty],
  );
  const total      = data?.total ?? 0;
  const emptyCount = rawRows.filter((r) => r.is_empty).length;

  const actions = (
    <div className="flex items-center gap-3">
      <label className="flex items-center gap-1.5 text-xs text-text-muted cursor-pointer select-none">
        <input
          type="checkbox"
          checked={showEmpty}
          onChange={(e) => setShowEmpty(e.target.checked)}
          className="accent-brand-500"
        />
        Show empty
        {emptyCount > 0 && <span className="text-text-muted/70">({emptyCount})</span>}
      </label>
      <Button
        variant="secondary"
        onClick={() => setOffset(Math.max(0, offset - PAGE_SIZE))}
        disabled={offset === 0}
      >
        Prev
      </Button>
      <Button
        variant="secondary"
        onClick={() => setOffset(offset + PAGE_SIZE)}
        disabled={offset + PAGE_SIZE >= total}
      >
        Next
      </Button>
    </div>
  );

  return (
    <Card title={`Spools (${total})`} actions={actions}>
      {isLoading && <div className="text-sm text-text-muted">Loading…</div>}
      {!isLoading && rows.length === 0 && rawRows.length === 0 && (
        <div className="text-sm text-text-muted py-8 text-center">
          No spools yet — scan a SpoolHard-tagged spool to add one.
        </div>
      )}
      {!isLoading && rows.length === 0 && rawRows.length > 0 && (
        <div className="text-sm text-text-muted py-8 text-center">
          All {rawRows.length} spools on this page are marked empty. Toggle
          "Show empty" to see them.
        </div>
      )}

      <div className="space-y-2">
        {rows.map((r) => <SpoolRow key={r.id} r={r} onDelete={() => del.mutate(r.id)} />)}
      </div>
    </Card>
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
        aria-expanded={open}
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
        {typeof r.weight_current === 'number' && r.weight_current >= 0 && (
          <div className="text-sm text-brand-400 font-mono tabular-nums">
            {r.weight_current}g
          </div>
        )}
        <button
          onClick={(e) => { e.stopPropagation(); onDelete(); }}
          className="text-text-muted hover:text-red-400 transition-colors cursor-pointer"
          aria-label="Delete spool"
        >
          <Trash2 size={16} />
        </button>
      </div>

      {open && <SpoolDetailPanel spool={r} />}
    </div>
  );
}
