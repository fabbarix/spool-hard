import { useMemo, useState } from 'react';
import { createPortal } from 'react-dom';
import { X, Link2, AlertTriangle } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import {
  useUserFilaments,
  useUpsertUserFilament,
  type UserFilament,
} from '../../hooks/useUserFilaments';

interface Props {
  // The AMS slot's tray_info_idx (e.g. "Pdb99855"). Written into the
  // chosen filament's `filament_id` on confirm.
  trayInfoIdx: string;
  // Slot context for the header copy. `material` filters the list to
  // matching filaments by default; `color` drives the small swatch.
  material:    string;
  color:       string;        // "RRGGBBAA"
  slotLabel:   string;
  onClose:     () => void;
}

// Modal: "Link <tray_info_idx> to a filament". Lists every user-filament
// row, defaults to the same-material subset so PLA spools don't surface
// PETG filaments in the picker, and on confirm PUT-overlays the chosen
// row's `filament_id` with this slot's tray_info_idx. After mutate
// success, the PrintersPanel's filamentByFilamentId map picks the row
// up immediately (react-query invalidates `user-filaments`), the
// unmatched affordance disappears, and any spool linked to that
// filament will ship the right tray_info_idx on its next AMS load.
export function LinkFilamentDialog({ trayInfoIdx, material, color, slotLabel, onClose }: Props) {
  const { data } = useUserFilaments();
  const upsert = useUpsertUserFilament();
  const [showAll, setShowAll] = useState(false);
  const [pendingId, setPendingId] = useState<string | null>(null);

  // Default filter: same material only. The Bambu MQTT report includes
  // `material` for the slot (PLA / PETG / etc.), which we trust as
  // ground-truth — there's no realistic case where the user wants to
  // associate a PLA preset with a PETG filament row. But the user-
  // filaments DB often has `filament_type` empty (Bambu Cloud sync
  // doesn't reliably carry it on custom presets), so we also include
  // untyped rows whose name mentions the material as a whole token —
  // catches "Colido PLA Basic" when matching PLA without surfacing
  // "Generic PETG - BIQU". The "Show all" toggle covers the rare case
  // where the heuristic misses.
  const rows = useMemo(() => {
    const all = (data?.rows ?? []);
    if (showAll || !material) return all;
    const want = material.toLowerCase();
    const tokenRe = new RegExp(`(?:^|[^A-Za-z0-9])${want}(?:[^A-Za-z0-9]|$)`, 'i');
    return all.filter((r) => {
      const t = (r.filament_type || '').toLowerCase();
      if (t === want) return true;
      if (!t && tokenRe.test(r.name || '')) return true;
      return false;
    });
  }, [data, showAll, material]);

  const hex = color && color.length >= 6 ? color.slice(0, 6) : '';
  const swatchBg = hex ? `#${hex}` : 'transparent';

  const pick = (f: UserFilament) => {
    setPendingId(f.setting_id);
    upsert.mutate(
      { setting_id: f.setting_id, filament_id: trayInfoIdx },
      {
        onSuccess: () => onClose(),
        onError: () => setPendingId(null),
      },
    );
  };

  return createPortal(
    <div
      className="fixed inset-0 z-[100] flex items-center justify-center bg-surface-body/80 backdrop-blur-sm animate-in fade-in"
      onClick={onClose}
    >
      <div
        className="flex flex-col gap-3 rounded-card border border-surface-border bg-surface-card p-4 w-[560px] max-w-[92vw] max-h-[80vh]"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-start justify-between gap-3">
          <div className="flex items-start gap-3 min-w-0">
            <div
              className="h-9 w-9 shrink-0 rounded border border-surface-border shadow-[inset_0_0_4px_rgba(0,0,0,0.4)]"
              style={{ background: swatchBg }}
              title={hex ? `#${hex}` : 'no color reported'}
            />
            <div className="min-w-0">
              <div className="text-sm font-medium text-text-primary">
                Link <span className="font-mono text-brand-400">{trayInfoIdx}</span> to a filament
              </div>
              <div className="text-xs text-text-muted truncate">
                {slotLabel} · {material || 'unknown material'} · the preset the printer reports for this slot
              </div>
            </div>
          </div>
          <button
            className="text-text-muted hover:text-text-primary transition-colors"
            onClick={onClose}
            aria-label="Close"
          >
            <X size={18} />
          </button>
        </div>

        {/* Explainer + filter toggle */}
        <div className="text-xs text-text-muted leading-relaxed">
          Pick the filament this preset code belongs to. The Bambu preset
          ID will be saved on the filament row so future spools you link
          to it ship the right slicer settings on the very first AMS load.
        </div>
        {material && (
          <label className="text-xs text-text-muted inline-flex items-center gap-2 -mt-1">
            <input
              type="checkbox"
              checked={showAll}
              onChange={(e) => setShowAll(e.target.checked)}
              className="rounded border-surface-border"
            />
            Show filaments of other materials too
          </label>
        )}

        {/* List */}
        <div className="flex-1 min-h-0 overflow-y-auto -mx-1 px-1">
          {rows.length === 0 ? (
            <div className="text-sm text-text-muted py-6 text-center">
              No {material ? `${material} ` : ''}filaments in your library yet.
              Add one under <span className="text-text-primary">Filaments</span> first, then come back.
            </div>
          ) : (
            <div className="flex flex-col gap-1.5">
              {rows.map((f) => {
                const alreadySet = !!f.filament_id;
                const sameId    = f.filament_id === trayInfoIdx;
                const pending   = pendingId === f.setting_id && upsert.isPending;
                return (
                  <button
                    key={f.setting_id}
                    type="button"
                    disabled={pending || sameId}
                    onClick={() => pick(f)}
                    className={`text-left rounded border border-surface-border bg-surface-input px-3 py-2 transition-colors hover:border-brand-400/60 disabled:opacity-50 disabled:cursor-not-allowed`}
                  >
                    <div className="flex items-center gap-2">
                      <div className="min-w-0 flex-1">
                        <div className="text-sm text-text-primary truncate">
                          {f.name}
                        </div>
                        <div className="text-[11px] text-text-muted font-mono truncate">
                          {[
                            f.filament_vendor,
                            f.filament_type,
                            f.filament_subtype || null,
                            f.filament_id ? `id:${f.filament_id}` : 'no id yet',
                          ].filter(Boolean).join(' · ')}
                        </div>
                      </div>
                      {sameId ? (
                        <span className="text-[10px] uppercase tracking-wider text-brand-400">
                          already linked
                        </span>
                      ) : alreadySet ? (
                        <span
                          className="inline-flex items-center gap-1 text-[10px] uppercase tracking-wider text-status-warning"
                          title={`Currently set to ${f.filament_id} — picking will overwrite`}
                        >
                          <AlertTriangle size={11} /> overwrite
                        </span>
                      ) : pending ? (
                        <span className="text-[10px] text-text-muted">saving…</span>
                      ) : (
                        <Link2 size={14} className="text-text-muted shrink-0" />
                      )}
                    </div>
                  </button>
                );
              })}
            </div>
          )}
        </div>

        {/* Footer */}
        <div className="flex items-center justify-between gap-2 pt-1">
          <div className="text-[11px] text-text-muted">
            {upsert.isError && <span className="text-status-error">Save failed — check device logs.</span>}
          </div>
          <Button variant="secondary" onClick={onClose}>Cancel</Button>
        </div>
      </div>
    </div>,
    document.body,
  );
}
