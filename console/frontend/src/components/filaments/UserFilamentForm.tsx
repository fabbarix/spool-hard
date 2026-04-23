import { useState } from 'react';
import { Plus, Trash2, BookOpen, X } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import { InputField } from '@spoolhard/ui/components/InputField';
import {
  useUpsertUserFilament,
  type UserFilament,
  type PaByNozzleEntry,
} from '../../hooks/useUserFilaments';
import { FilamentPicker } from '../spools/FilamentPicker';
import type { FilamentEntry } from '../../hooks/useFilamentsDb';

// Minimal create/edit form for a user filament preset. Fields mirror the
// shape stored in user_filaments.jsonl on the device (see
// console/firmware/include/filament_record.h). Anything left blank
// becomes the firmware's "unset" sentinel (-1 for ints, 0 for floats).
//
// The setting_id field isn't editable — it's server-generated on create
// (PFUL<hash>) and immutable on update.
//
// Two ergonomic affordances on top of a plain form:
//   * "Base on a stock filament" — opens the FilamentPicker (stockOnly)
//     and prefills name / vendor / temps / density / base_id / etc from
//     the chosen preset. Saves the user from typing GFSA00 + remembering
//     the right tray_info_idx for, say, a Bambu PLA Basic clone.
//   * Per-nozzle pressure-advance table — PA depends on (filament,
//     nozzle), so we let the user record one K per nozzle they own. The
//     scalar `pressure_advance` field stays as a fallback for nozzles
//     not in the table (and for cloud round-trip — Bambu only stores the
//     scalar).
export function UserFilamentForm({
  initial, onClose,
}: {
  initial?: UserFilament;
  onClose: () => void;
}) {
  const upsert = useUpsertUserFilament();
  const [form, setForm] = useState<Partial<UserFilament>>({
    name:             initial?.name             ?? '',
    base_id:          initial?.base_id          ?? 'GFSA00',  // generic PLA base
    filament_type:    initial?.filament_type    ?? 'PLA',
    filament_subtype: initial?.filament_subtype ?? 'basic',
    filament_vendor:  initial?.filament_vendor  ?? '',
    filament_id:      initial?.filament_id      ?? '',
    nozzle_temp_min:  initial?.nozzle_temp_min  ?? 200,
    nozzle_temp_max:  initial?.nozzle_temp_max  ?? 220,
    density:          initial?.density          ?? 1.24,
    pressure_advance: initial?.pressure_advance ?? 0.04,
    pa_by_nozzle:     initial?.pa_by_nozzle     ?? [],
  });
  const [pickerOpen, setPickerOpen] = useState(false);

  const set = <K extends keyof UserFilament>(k: K, v: UserFilament[K]) =>
    setForm((f) => ({ ...f, [k]: v }));

  // Take a stock-library entry and overlay every field it actually
  // populates. Leaves the user's name alone if they've started typing
  // (rather than blowing it away mid-edit), but otherwise prefills.
  const applyStockBase = (e: FilamentEntry) => {
    setForm((f) => ({
      ...f,
      name:             f.name && f.name.trim() ? f.name : e.name,
      base_id:          e.base_id          || f.base_id,
      filament_type:    e.material         || f.filament_type,
      filament_subtype: e.subtype          || f.filament_subtype,
      filament_vendor:  e.brand            || f.filament_vendor,
      filament_id:      e.filament_id      || f.filament_id,
      nozzle_temp_min:  typeof e.nozzle_temp_min === 'number' ? e.nozzle_temp_min : f.nozzle_temp_min,
      nozzle_temp_max:  typeof e.nozzle_temp_max === 'number' ? e.nozzle_temp_max : f.nozzle_temp_max,
      density:          typeof e.density === 'number' && e.density > 0 ? e.density : f.density,
      pressure_advance: typeof e.pressure_advance === 'number' && e.pressure_advance > 0
        ? e.pressure_advance
        : f.pressure_advance,
    }));
  };

  // Per-nozzle PA mutation helpers. The list is small (at most a few
  // rows per user) so we keep it as state and overwrite-replace on save.
  const pa = form.pa_by_nozzle ?? [];
  const updatePa = (idx: number, patch: Partial<PaByNozzleEntry>) => {
    const next = pa.map((e, i) => (i === idx ? { ...e, ...patch } : e));
    set('pa_by_nozzle', next);
  };
  const addPa = () => set('pa_by_nozzle', [...pa, { nozzle: 0.4, k: form.pressure_advance ?? 0 }]);
  const delPa = (idx: number) => set('pa_by_nozzle', pa.filter((_, i) => i !== idx));

  const onSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (!form.name) return;
    upsert.mutate(
      { ...form, setting_id: initial?.setting_id },
      { onSuccess: onClose },
    );
  };

  return (
    <form
      onSubmit={onSubmit}
      className="rounded-md border border-surface-border bg-surface-input p-4 space-y-3"
    >
      <div className="flex items-center justify-between">
        <h3 className="text-sm font-semibold text-text-primary">
          {initial ? 'Edit filament' : 'New filament'}
        </h3>
        <div className="flex items-center gap-2">
          {initial && (
            <span className="text-xs font-mono text-text-muted">{initial.setting_id}</span>
          )}
          <Button
            variant="secondary"
            type="button"
            onClick={() => setPickerOpen(true)}
            className="!py-1 !px-2"
          >
            <BookOpen size={12} className="mr-1.5 inline" />
            <span className="text-xs">Base on a stock filament</span>
          </Button>
        </div>
      </div>
      <div className="grid grid-cols-2 gap-3">
        <InputField label="Name"   value={form.name ?? ''}   onChange={(e) => set('name',   e.target.value)} />
        <InputField label="Vendor" value={form.filament_vendor ?? ''} onChange={(e) => set('filament_vendor', e.target.value)} />
        <InputField label="Material (PLA / PETG / TPU / …)" value={form.filament_type ?? ''} onChange={(e) => set('filament_type', e.target.value.toUpperCase())} />
        <InputField label="Subtype (basic / matte / …)" value={form.filament_subtype ?? ''} onChange={(e) => set('filament_subtype', e.target.value)} />
        <InputField label="Bambu base ID" value={form.base_id ?? ''} onChange={(e) => set('base_id', e.target.value)} />
        <InputField label="Bambu filament ID (tray_info_idx)" value={form.filament_id ?? ''} onChange={(e) => set('filament_id', e.target.value)} />
        <InputField label="Nozzle temp min (°C)" type="number" value={String(form.nozzle_temp_min ?? '')} onChange={(e) => set('nozzle_temp_min', Number(e.target.value))} />
        <InputField label="Nozzle temp max (°C)" type="number" value={String(form.nozzle_temp_max ?? '')} onChange={(e) => set('nozzle_temp_max', Number(e.target.value))} />
        <InputField label="Density (g/cm³)" type="number" step={0.01} value={String(form.density ?? '')} onChange={(e) => set('density', Number(e.target.value))} />
        <InputField label="Pressure advance — default (K)" type="number" step={0.001} value={String(form.pressure_advance ?? '')} onChange={(e) => set('pressure_advance', Number(e.target.value))} />
      </div>

      <div className="border-t border-surface-border pt-3 space-y-2">
        <div className="flex items-center justify-between">
          <div className="text-xs font-semibold uppercase tracking-wider text-text-secondary">
            Pressure advance per nozzle
          </div>
          <Button variant="secondary" type="button" onClick={addPa} className="!py-1 !px-2">
            <Plus size={12} className="mr-1 inline" />
            <span className="text-xs">Add nozzle</span>
          </Button>
        </div>
        {pa.length === 0 ? (
          <div className="text-[11px] text-text-muted italic">
            No per-nozzle entries yet — the default K above is used for every nozzle.
            Add an entry to override K for a specific nozzle (e.g. 0.6 mm).
          </div>
        ) : (
          <table className="w-full text-xs tabular-nums">
            <thead className="text-text-muted">
              <tr className="text-left">
                <th className="font-normal py-1 pr-3">Nozzle (mm)</th>
                <th className="font-normal py-1 pr-3">K</th>
                <th className="font-normal py-1 pr-3 text-right"></th>
              </tr>
            </thead>
            <tbody>
              {pa.map((e, i) => (
                <tr key={i} className="border-t border-surface-border/50">
                  <td className="py-1 pr-3 w-32">
                    <input
                      type="number"
                      step={0.1}
                      value={e.nozzle}
                      onChange={(ev) => updatePa(i, { nozzle: Number(ev.target.value) })}
                      className="w-full bg-surface-card border border-surface-border rounded px-2 py-1 font-mono text-xs"
                    />
                  </td>
                  <td className="py-1 pr-3 w-32">
                    <input
                      type="number"
                      step={0.001}
                      value={e.k}
                      onChange={(ev) => updatePa(i, { k: Number(ev.target.value) })}
                      className="w-full bg-surface-card border border-surface-border rounded px-2 py-1 font-mono text-xs"
                    />
                  </td>
                  <td className="py-1 text-right">
                    <button
                      type="button"
                      onClick={() => delPa(i)}
                      className="text-text-muted hover:text-red-400 transition-colors"
                      aria-label="Remove nozzle entry"
                    >
                      <Trash2 size={12} />
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      <div className="flex gap-2">
        <Button type="submit" disabled={upsert.isPending || !form.name}>
          {upsert.isPending ? 'Saving…' : (initial ? 'Save' : 'Create')}
        </Button>
        <Button variant="secondary" type="button" onClick={onClose} disabled={upsert.isPending}>
          <X size={14} className="mr-1 inline" /> Cancel
        </Button>
        {upsert.isError && (
          <div className="text-sm text-red-400 self-center">
            {(upsert.error as Error)?.message || 'Failed to save'}
          </div>
        )}
      </div>

      {pickerOpen && (
        <FilamentPicker
          stockOnly
          title="Base on a stock filament"
          description="Pick a stock library preset to prefill base_id, vendor, temps, density and PA. You can edit anything afterwards."
          onPick={(e) => applyStockBase(e)}
          onClose={() => setPickerOpen(false)}
        />
      )}
    </form>
  );
}
