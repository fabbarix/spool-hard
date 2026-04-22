import { useState, useMemo } from 'react';
import { X, Check, Eraser } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import { useSpools, type SpoolRecord } from '../../hooks/useSpools';
import { useSetAmsMapping } from '../../hooks/usePrinters';

interface Props {
  serial: string;
  amsUnit: number;
  slotId: number;
  currentSpoolId: string;
  isOverride: boolean;
  onClose: () => void;
}

// Modal picker for manually assigning a spool to an AMS slot. This is the
// "drag-drop" feature from the plan, implemented as a click-to-pick because
// the dashboard and the spool list live on different tabs — HTML5 drag-drop
// doesn't span routes — and click is keyboard/touch friendly for free.
//
// Opens centered, backdrop click closes, Escape closes. Lists all known
// spools with a search filter; picking one POSTs the override and closes.
export function AmsSlotPicker({ serial, amsUnit, slotId, currentSpoolId, isOverride, onClose }: Props) {
  const { data } = useSpools(0, 500);   // 500 > any plausible inventory
  const setMapping = useSetAmsMapping();
  const [q, setQ] = useState('');

  const rows = useMemo(() => {
    const all = data?.rows ?? [];
    const needle = q.trim().toLowerCase();
    if (!needle) return all;
    return all.filter((r) =>
      (r.brand ?? '').toLowerCase().includes(needle) ||
      (r.material_type ?? '').toLowerCase().includes(needle) ||
      (r.color_name ?? '').toLowerCase().includes(needle) ||
      (r.id ?? '').toLowerCase().includes(needle)
    );
  }, [data, q]);

  const assign = (spool_id: string) => {
    setMapping.mutate({ serial, ams_unit: amsUnit, slot_id: slotId, spool_id }, {
      onSuccess: () => onClose(),
    });
  };
  const clear = () => assign('');

  return (
    <div
      className="fixed inset-0 z-[90] flex items-center justify-center bg-surface-body/80 backdrop-blur-sm animate-in fade-in"
      onClick={onClose}
    >
      <div
        className="flex flex-col gap-3 rounded-card border border-surface-border bg-surface-card p-4 w-[560px] max-w-[92vw] max-h-[80vh]"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between">
          <div>
            <div className="text-sm font-medium text-text-primary">
              Assign spool to {amsUnit === 254 ? 'External' : `AMS ${amsUnit + 1}/${slotId + 1}`}
            </div>
            <div className="text-[11px] text-text-muted mt-0.5">
              Printer <span className="font-mono">{serial}</span>
            </div>
          </div>
          <button onClick={onClose} className="text-text-muted hover:text-text-primary p-1">
            <X size={16} />
          </button>
        </div>

        <input
          type="text"
          placeholder="Search brand / material / color / id…"
          value={q}
          onChange={(e) => setQ(e.target.value)}
          autoFocus
          className="w-full bg-surface-input border border-surface-border rounded-input px-3 py-2 text-sm text-text-primary placeholder:text-text-muted"
        />

        <div className="flex-1 overflow-y-auto min-h-0">
          {rows.length === 0 ? (
            <div className="text-sm text-text-muted text-center py-8">No matching spools.</div>
          ) : (
            <ul className="space-y-1">
              {rows.map((r) => (
                <SpoolPickRow
                  key={r.id}
                  r={r}
                  active={r.id === currentSpoolId}
                  onPick={() => assign(r.id)}
                />
              ))}
            </ul>
          )}
        </div>

        <div className="flex items-center justify-between pt-2 border-t border-surface-border">
          <Button
            variant="secondary"
            onClick={clear}
            disabled={!isOverride || setMapping.isPending}
            title={isOverride ? 'Remove manual mapping, fall back to tag_uid matching' : 'No manual mapping to clear'}
          >
            <Eraser size={14} className="mr-1.5 inline" />
            Clear manual mapping
          </Button>
          {setMapping.error instanceof Error && (
            <span className="text-xs text-status-error">{setMapping.error.message}</span>
          )}
        </div>
      </div>
    </div>
  );
}

function SpoolPickRow({ r, active, onPick }: { r: SpoolRecord; active: boolean; onPick: () => void }) {
  const bg = r.color_code ? `#${r.color_code.slice(0, 6)}` : '#444';
  return (
    <li>
      <button
        onClick={onPick}
        className={`w-full flex items-center gap-3 p-2 rounded-md text-left transition-colors ${
          active ? 'bg-brand-500/15 text-brand-400' : 'hover:bg-surface-card-hover text-text-primary'
        }`}
      >
        <span className="w-3 h-3 rounded-full flex-shrink-0 border border-surface-border" style={{ background: bg }} />
        <span className="flex-1 min-w-0">
          <div className="text-sm truncate">
            {r.brand || 'Unknown'} · {r.material_type || '—'}
            {r.color_name && <span className="text-text-muted"> · {r.color_name}</span>}
          </div>
          <div className="text-[11px] text-text-muted font-mono truncate">{r.id}</div>
        </span>
        {typeof r.weight_current === 'number' && r.weight_current >= 0 && (
          <span className="font-mono text-xs text-text-muted tabular-nums">{r.weight_current}g</span>
        )}
        {active && <Check size={14} className="text-brand-400 flex-shrink-0" />}
      </button>
    </li>
  );
}
