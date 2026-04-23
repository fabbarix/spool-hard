import { useMemo, useState } from 'react';
import { X, Thermometer, Search, BookOpen, User as UserIcon } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import { useFilamentsDb, type FilamentEntry } from '../../hooks/useFilamentsDb';
import { useUserFilaments } from '../../hooks/useUserFilaments';

interface Props {
  onPick: (entry: FilamentEntry) => void;
  onClose: () => void;
  // When true, hide user filaments and show stock only. Used by the
  // "Base on a stock filament" picker in the custom-filament form so the
  // user doesn't end up basing one custom on another by accident.
  stockOnly?: boolean;
  // Optional copy override — the default is geared at spool editing
  // ("Pre-fill material, subtype, temps…"), but reuse contexts may want
  // to phrase it differently.
  title?: string;
  description?: string;
}

// Pill filter for material families. "Other" is a catch-all that also
// surfaces custom brands (e.g. Generic, Overture) when the user uploads a
// non-Bambu-only DB.
const FAMILIES = ['All', 'PLA', 'PETG', 'ABS', 'ASA', 'TPU', 'PA', 'PC', 'Other'] as const;
type Family = typeof FAMILIES[number];

export function FilamentPicker({ onPick, onClose, stockOnly, title, description }: Props) {
  const { data, isLoading, error } = useFilamentsDb();
  const userQ = useUserFilaments();
  const [q, setQ]         = useState('');
  const [fam, setFam]     = useState<Family>('All');

  // Tag stock entries with source=stock so the renderer + applyFilament
  // can tell them apart from user entries (which carry a setting_id).
  const stockEntries = (data?.entries ?? []).map<FilamentEntry>((e) => ({
    ...e, source: 'stock',
  }));
  // Map user filaments into the same FilamentEntry shape — slots into
  // the existing search / filter / row UI without forking it.
  const userEntries: FilamentEntry[] = stockOnly
    ? []
    : (userQ.data?.rows ?? []).map((u) => ({
        name:        u.name || '(unnamed)',
        filament_id: u.filament_id || '',
        brand:       u.filament_vendor || '',
        material:    u.filament_type   || '',
        subtype:     u.filament_subtype || '',
        nozzle_temp_min: u.nozzle_temp_min > 0 ? u.nozzle_temp_min : undefined,
        nozzle_temp_max: u.nozzle_temp_max > 0 ? u.nozzle_temp_max : undefined,
        density:     u.density > 0 ? u.density : undefined,
        pressure_advance: u.pressure_advance > 0 ? u.pressure_advance : undefined,
        setting_id:  u.setting_id,
        source:      'user',
      }));
  // User filaments first — they're typically more relevant to the user
  // who took the time to create them.
  const entries = [...userEntries, ...stockEntries];
  const present = entries.length > 0;

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase();
    return entries.filter((e) => {
      if (fam !== 'All') {
        const m = e.material.toUpperCase();
        if (fam === 'Other') {
          if (['PLA', 'PETG', 'ABS', 'ASA', 'TPU', 'PA', 'PC'].some((x) => m.startsWith(x))) return false;
        } else if (!m.startsWith(fam)) {
          return false;
        }
      }
      if (!needle) return true;
      return (
        e.name.toLowerCase().includes(needle) ||
        e.brand.toLowerCase().includes(needle) ||
        e.material.toLowerCase().includes(needle) ||
        e.subtype.toLowerCase().includes(needle) ||
        e.filament_id.toLowerCase().includes(needle)
      );
    });
  }, [entries, q, fam]);

  return (
    <div
      className="fixed inset-0 z-[90] flex items-center justify-center bg-surface-body/80 backdrop-blur-sm animate-in fade-in"
      onClick={onClose}
    >
      <div
        className="flex flex-col gap-3 rounded-card border border-surface-border bg-surface-card p-4 w-[640px] max-w-[94vw] max-h-[82vh]"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-2">
            <BookOpen size={16} className="text-brand-400" />
            <div>
              <div className="text-sm font-medium text-text-primary">{title ?? 'Load from filaments library'}</div>
              <div className="text-[11px] text-text-muted mt-0.5">
                {description ?? 'Pre-fill material, subtype, temps and Bambu filament ID from an uploaded bambu-filaments database.'}
              </div>
            </div>
          </div>
          <button onClick={onClose} className="text-text-muted hover:text-text-primary p-1" aria-label="Close">
            <X size={16} />
          </button>
        </div>

        {/* Empty / error / loading states up top so the filter UI stays hidden
            when there's nothing to filter through. */}
        {!present && !isLoading && !error && (
          <div className="text-sm text-text-muted text-center py-10 border border-dashed border-surface-border rounded-md">
            No library uploaded yet. <br/>
            Upload a <span className="font-mono">filaments.jsonl</span> in
            <span className="text-text-primary"> Config → Device → Filaments library</span>.
          </div>
        )}
        {error instanceof Error && (
          <div className="text-sm text-status-error text-center py-6">{error.message}</div>
        )}
        {isLoading && (
          <div className="text-sm text-text-muted text-center py-6">Loading library…</div>
        )}

        {present && !isLoading && !error && (
          <>
            {/* Family chips */}
            <div className="flex flex-wrap gap-1">
              {FAMILIES.map((f) => (
                <button
                  key={f}
                  onClick={() => setFam(f)}
                  className={`px-2.5 py-1 text-xs rounded-full border transition-colors ${
                    fam === f
                      ? 'bg-brand-500/15 text-brand-400 border-brand-500/30'
                      : 'text-text-muted border-surface-border hover:bg-surface-card-hover'
                  }`}
                >
                  {f}
                </button>
              ))}
            </div>

            {/* Search */}
            <div className="relative">
              <Search size={14} className="absolute left-3 top-1/2 -translate-y-1/2 text-text-muted" />
              <input
                type="text"
                placeholder="Search name / brand / material / filament id…"
                value={q}
                onChange={(e) => setQ(e.target.value)}
                autoFocus
                className="w-full bg-surface-input border border-surface-border rounded-input pl-9 pr-3 py-2 text-sm text-text-primary placeholder:text-text-muted"
              />
            </div>

            {/* Result count / truncation notice */}
            <div className="text-[11px] text-text-muted">
              {rows.length} of {entries.length} presets
            </div>

            {/* Results */}
            <div className="flex-1 overflow-y-auto min-h-0 -mx-1 px-1">
              {rows.length === 0 ? (
                <div className="text-sm text-text-muted text-center py-8">No matching presets.</div>
              ) : (
                <ul className="space-y-1">
                  {/* Cap at 250 so a no-filter query doesn't render a thousand
                      rows into the DOM; the count label above still shows the
                      full match total so the user knows to narrow the search. */}
                  {rows.slice(0, 250).map((e) => (
                    <FilamentRow key={e.name} e={e} onPick={() => { onPick(e); onClose(); }} />
                  ))}
                </ul>
              )}
            </div>
          </>
        )}

        <div className="flex items-center justify-end pt-2 border-t border-surface-border">
          <Button variant="secondary" onClick={onClose}>Cancel</Button>
        </div>
      </div>
    </div>
  );
}

function FilamentRow({ e, onPick }: { e: FilamentEntry; onPick: () => void }) {
  const bg = e.color_code ? `#${e.color_code.slice(0, 6)}` : 'transparent';
  const hasTemps = typeof e.nozzle_temp_min === 'number' && typeof e.nozzle_temp_max === 'number';
  return (
    <li>
      <button
        onClick={onPick}
        className="w-full flex items-center gap-3 p-2 rounded-md text-left transition-colors hover:bg-surface-card-hover"
      >
        <span
          className="w-3.5 h-3.5 rounded-full flex-shrink-0 border border-surface-border"
          style={{ background: bg }}
          aria-hidden
        />
        <span className="flex-1 min-w-0">
          <div className="text-sm text-text-primary truncate flex items-center gap-2">
            {e.source === 'user' && (
              <UserIcon size={11} className="text-brand-400 flex-shrink-0" aria-label="Custom filament" />
            )}
            {e.brand || 'Unknown'} · {e.material || '—'}
            {e.subtype && <span className="text-text-muted"> · {e.subtype}</span>}
          </div>
          <div className="text-[11px] text-text-muted font-mono truncate">
            {e.filament_id && <span>{e.filament_id} · </span>}{e.name}
          </div>
        </span>
        {hasTemps && (
          <span className="flex items-center gap-1 text-[11px] text-text-muted font-mono tabular-nums flex-shrink-0">
            <Thermometer size={11} />
            {e.nozzle_temp_min}–{e.nozzle_temp_max}°C
          </span>
        )}
      </button>
    </li>
  );
}
