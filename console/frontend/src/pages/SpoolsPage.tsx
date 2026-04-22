import { useEffect, useState } from 'react';
import { Trash2, ChevronDown, ChevronRight, Gauge, Package, Scale } from 'lucide-react';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';
import { SubTabBar, type SubTab } from '@spoolhard/ui/components/SubTabBar';
import { useSpools, useSpoolDelete, type SpoolRecord } from '../hooks/useSpools';
import { SpoolDetailPanel } from '../components/spools/SpoolDetailPanel';
import { CoreWeightsSection } from '../components/config/CoreWeightsSection';

const PAGE_SIZE = 25;

type SpoolsTab = 'list' | 'core-weights';

const tabs: SubTab<SpoolsTab>[] = [
  { id: 'list',         label: 'Spools',       icon: <Package size={14} /> },
  { id: 'core-weights', label: 'Core weights', icon: <Scale   size={14} /> },
];

function getInitialTab(): SpoolsTab {
  const params = new URLSearchParams(window.location.search);
  const t = params.get('tab');
  if (t && tabs.some((tab) => tab.id === t)) return t as SpoolsTab;
  return 'list';
}

export function SpoolsPage() {
  const [activeTab, setActiveTab] = useState<SpoolsTab>(getInitialTab);

  const navigate = (t: SpoolsTab) => {
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
        {activeTab === 'list'         && <SpoolsListSection />}
        {activeTab === 'core-weights' && <CoreWeightsSection />}
      </div>
    </div>
  );
}

function SpoolsListSection() {
  const [offset, setOffset] = useState(0);
  const { data, isLoading } = useSpools(offset, PAGE_SIZE);
  const del = useSpoolDelete();

  const rows = data?.rows ?? [];
  const total = data?.total ?? 0;

  const actions = (
    <>
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
    </>
  );

  return (
    <Card title={`Spools (${total})`} actions={actions}>
      {isLoading && <div className="text-sm text-text-muted">Loading…</div>}
      {!isLoading && rows.length === 0 && (
        <div className="text-sm text-text-muted py-8 text-center">
          No spools yet — scan a SpoolHard-tagged spool to add one.
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
          <div className="text-sm font-medium text-text-primary truncate flex items-center gap-2">
            {r.brand || 'Unknown'} · {r.material_type || '—'}
            {r.material_subtype && ` (${r.material_subtype})`}
            {r.color_name && ` · ${r.color_name}`}
            {kValues.length > 0 && (
              <span className="inline-flex items-center gap-1 text-[10px] uppercase tracking-wider text-brand-400">
                <Gauge size={10} /> K ×{kValues.length}
              </span>
            )}
          </div>
          <div className="text-xs text-text-muted font-mono truncate">
            {r.tag_id} · {r.data_origin || 'Unknown'}
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
