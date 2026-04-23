import { useMemo, useState } from 'react';
import { Plus, Trash2, BookOpen, X, RotateCcw } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import { InputField } from '@spoolhard/ui/components/InputField';
import {
  useUpsertUserFilament,
  useCloudFilamentByName,
  cloudBodyToFilamentEntry,
  resolveUserFilament,
  type UserFilament,
  type PaByNozzleEntry,
} from '../../hooks/useUserFilaments';
import { useFilamentsDb, type FilamentEntry } from '../../hooks/useFilamentsDb';
import { FilamentPicker } from '../spools/FilamentPicker';

// Create/edit form for a user filament preset. Fields mirror the
// storage shape (see console/firmware/include/filament_record.h).
//
// Inheritance model:
//   - Picking "Base on a stock filament" sets `parent_setting_id` to
//     the picked stock entry's setting_id. It also fills profile-
//     IDENTITY fields (name if blank, base_id, vendor-if-blank,
//     filament_type, subtype, filament_id) from the stock entry.
//   - The INHERITABLE numeric/PA fields (nozzle temps, density, K)
//     are deliberately NOT copied on pick — they stay empty in the
//     form, with the parent's values shown as placeholders. Saved
//     empty fields persist as sentinel values (-1 / 0 / []) and the
//     display layer falls back to the parent. That way editing the
//     base_id later (or the stock library updating) flows new values
//     through without wiping the user's explicit overrides.
//   - A value the user types IS an override and is persisted on save.
//     A per-field "↺ Reset" button next to the label clears the
//     override back to "inherit from parent".
export function UserFilamentForm({
  initial, onClose,
}: {
  initial?: UserFilament;
  onClose: () => void;
}) {
  const upsert  = useUpsertUserFilament();
  const stockDb = useFilamentsDb();

  // A Partial is enough — empty/unset means "inherit from parent".
  const [form, setForm] = useState<Partial<UserFilament>>({
    setting_id:        initial?.setting_id,
    name:              initial?.name ?? '',
    base_id:           initial?.base_id ?? '',
    parent_setting_id: initial?.parent_setting_id ?? '',
    filament_type:     initial?.filament_type ?? '',
    filament_subtype:  initial?.filament_subtype ?? '',
    filament_vendor:   initial?.filament_vendor ?? '',
    filament_id:       initial?.filament_id ?? '',
    nozzle_temp_min:   initial?.nozzle_temp_min,
    nozzle_temp_max:   initial?.nozzle_temp_max,
    density:           initial?.density,
    pressure_advance:  initial?.pressure_advance,
    pa_by_nozzle:      initial?.pa_by_nozzle ?? [],
  });
  const [pickerOpen, setPickerOpen] = useState(false);

  const set = <K extends keyof UserFilament>(k: K, v: UserFilament[K] | undefined) =>
    setForm((f) => ({ ...f, [k]: v }));

  // Look up the picked parent in the local stock library so we can
  // show inherited values as placeholders throughout the form.
  const stockParent: FilamentEntry | null = useMemo(() => {
    const id = form.parent_setting_id;
    if (!id || !stockDb.data?.entries) return null;
    return stockDb.data.entries.find((e) => e.setting_id === id) ?? null;
  }, [form.parent_setting_id, stockDb.data]);

  // Cloud-side parent (Bambu's `inherits` chain). Captured into
  // `cloud_inherits` during sync — for cloud-synced customs this is
  // typically the @<printer>-variant preset, which is NOT in our local
  // stock library (the build pipeline only emits @base entries). The
  // public-catalog cache makes this lookup fast after the first hit.
  // Only fetched when we have no stock parent — if the user explicitly
  // picked a local stock parent via the picker, we honour that choice.
  const cloudInherits = form.cloud_inherits ?? '';
  const cloudParentQ = useCloudFilamentByName(
    cloudInherits, !!cloudInherits && !stockParent,
  );
  const cloudParent: FilamentEntry | null = useMemo(() => {
    if (cloudParentQ.data?.status !== 'ok') return null;
    return cloudBodyToFilamentEntry(cloudParentQ.data.body);
  }, [cloudParentQ.data]);

  // Stock parent (explicit user pick) wins; cloud parent (implicit via
  // sync) is the fallback. The resolver only consumes one parent, so
  // we pick which to surface.
  const parent = stockParent ?? cloudParent;

  // Resolved view drives the placeholder text — when the user clears a
  // field we show "1.24 (from Bambu PLA Basic @base)" so the user knows
  // what value will apply at print time.
  const resolved = useMemo(
    () => resolveUserFilament(form as UserFilament, parent),
    [form, parent],
  );

  const applyStockBase = (e: FilamentEntry) => {
    setForm((f) => ({
      ...f,
      // Link, don't copy, for inheritable fields. The resolved view +
      // placeholders convey what the values will be without persisting
      // them to the custom record.
      parent_setting_id: e.setting_id ?? '',
      // Profile-identity fields: safe to fill. Name gets filled only
      // when blank so the user can start typing "My PLA" first and
      // then pick a base without losing their in-progress name.
      name:              f.name && f.name.trim() ? f.name : `${e.name} (custom)`,
      base_id:           e.base_id        || f.base_id,
      filament_type:     e.material       || f.filament_type,
      filament_subtype:  e.subtype        || f.filament_subtype,
      filament_vendor:   e.brand          || f.filament_vendor,
      filament_id:       e.filament_id    || f.filament_id,
      // Explicitly DO NOT copy nozzle temps / density / PA. They stay
      // unset in the form so the resolver sources them from the parent.
    }));
  };

  // Inheritable numeric fields — helpers centralise the "what does the
  // input show / does it render as inherited" logic so every row stays
  // in sync.
  const numericInput = (key: 'nozzle_temp_min' | 'nozzle_temp_max' | 'density' | 'pressure_advance') => {
    const current = form[key] as number | undefined;
    const isUnset = current === undefined || current < 0 || (key === 'density' || key === 'pressure_advance' ? current === 0 : false);
    const inherited = resolved.inherited[key];
    const placeholderValue = inherited ? resolved[key] : null;
    return {
      value: isUnset ? '' : String(current),
      placeholder: placeholderValue !== null && placeholderValue > 0
        ? (parent ? `${placeholderValue} (from ${parent.name})` : String(placeholderValue))
        : undefined,
      onChange: (ev: React.ChangeEvent<HTMLInputElement>) => {
        // Empty string = reset to inherit; typed value = override.
        if (ev.target.value === '') set(key, undefined);
        else set(key, Number(ev.target.value));
      },
      onReset: () => set(key, undefined),
      canReset: !isUnset && !!parent,
      inherited,
    };
  };

  // Per-nozzle PA mutation helpers. The list itself is one override —
  // if the user clears the table the custom inherits (currently parent
  // never has a PA-per-nozzle table, but the path is future-proof).
  const pa = form.pa_by_nozzle ?? [];
  const updatePa = (idx: number, patch: Partial<PaByNozzleEntry>) => {
    const next = pa.map((e, i) => (i === idx ? { ...e, ...patch } : e));
    set('pa_by_nozzle', next);
  };
  const addPa = () => set('pa_by_nozzle', [...pa, {
    nozzle: 0.4,
    k: resolved.pressure_advance > 0 ? resolved.pressure_advance : 0,
  }]);
  const delPa = (idx: number) => set('pa_by_nozzle', pa.filter((_, i) => i !== idx));

  const onSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (!form.name) return;
    // Translate undefined → sentinel so the firmware stores "unset" and
    // the display layer falls back to the parent. Matches FilamentRecord
    // defaults on the firmware side.
    const body: Partial<UserFilament> = {
      ...form,
      nozzle_temp_min:  form.nozzle_temp_min  ?? -1,
      nozzle_temp_max:  form.nozzle_temp_max  ?? -1,
      density:          form.density          ?? 0,
      pressure_advance: form.pressure_advance ?? 0,
      setting_id:       initial?.setting_id,
    };
    upsert.mutate(body, { onSuccess: onClose });
  };

  const nMin = numericInput('nozzle_temp_min');
  const nMax = numericInput('nozzle_temp_max');
  const dens = numericInput('density');
  const paK  = numericInput('pressure_advance');

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

      {parent && (
        <div className="rounded-md border border-brand-500/30 bg-brand-500/5 px-2 py-1.5 text-[11px] text-text-muted">
          Inheriting from <span className="text-brand-400 font-mono">{parent.name}</span>
          {' '}— empty fields below will use the parent's values at print time.
        </div>
      )}

      <div className="grid grid-cols-2 gap-3">
        <InputField label="Name"   value={form.name ?? ''}   onChange={(e) => set('name',   e.target.value)} />
        <InputField
          label={fieldLabel('Vendor', !form.filament_vendor && !!parent?.brand)}
          value={form.filament_vendor ?? ''}
          placeholder={!form.filament_vendor && parent?.brand ? `${parent.brand} (from parent)` : undefined}
          onChange={(e) => set('filament_vendor', e.target.value)}
        />
        <InputField
          label={fieldLabel('Material (PLA / PETG / TPU / …)', !form.filament_type && !!parent?.material)}
          value={form.filament_type ?? ''}
          placeholder={!form.filament_type && parent?.material ? `${parent.material} (from parent)` : undefined}
          onChange={(e) => set('filament_type', e.target.value.toUpperCase())}
        />
        <InputField
          label={fieldLabel('Subtype (basic / matte / …)', !form.filament_subtype && !!parent?.subtype)}
          value={form.filament_subtype ?? ''}
          placeholder={!form.filament_subtype && parent?.subtype ? `${parent.subtype} (from parent)` : undefined}
          onChange={(e) => set('filament_subtype', e.target.value)}
        />
        <InputField label="Bambu base ID" value={form.base_id ?? ''} onChange={(e) => set('base_id', e.target.value)} />
        <InputField
          label={fieldLabel('Bambu filament ID (tray_info_idx)', !form.filament_id && !!parent?.filament_id)}
          value={form.filament_id ?? ''}
          placeholder={!form.filament_id && parent?.filament_id ? `${parent.filament_id} (from parent)` : undefined}
          onChange={(e) => set('filament_id', e.target.value)}
        />
        <NumericField label="Nozzle temp min (°C)" helper={nMin} />
        <NumericField label="Nozzle temp max (°C)" helper={nMax} />
        <NumericField label="Density (g/cm³)"      helper={dens} step={0.01} />
        <NumericField label="Pressure advance — default (K)" helper={paK} step={0.001} />
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
          description="Pick a stock preset as the parent. Values the custom doesn't set will be inherited — you only need to override what you want to change."
          onPick={(e) => applyStockBase(e)}
          onClose={() => setPickerOpen(false)}
        />
      )}
    </form>
  );
}

// Adds a subtle "• inherited" decoration to a field label so the user
// can see at a glance which fields are currently coming from the parent.
function fieldLabel(text: string, inherited: boolean): string {
  return inherited ? `${text} · inherited` : text;
}

// Inheritable numeric input with a one-click "↺" button that resets
// the field back to "inherit from parent" (i.e. clears the override so
// the placeholder value applies). Keeps the form grid aligned with the
// non-inheritable fields by reusing InputField for the input itself.
interface NumericHelper {
  value: string;
  placeholder?: string;
  onChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
  onReset: () => void;
  canReset: boolean;
  inherited: boolean;
}
function NumericField({ label, helper, step }: { label: string; helper: NumericHelper; step?: number }) {
  return (
    <div className="relative">
      <InputField
        label={fieldLabel(label, helper.inherited)}
        type="number"
        step={step}
        value={helper.value}
        placeholder={helper.placeholder}
        onChange={helper.onChange}
      />
      {helper.canReset && (
        <button
          type="button"
          onClick={helper.onReset}
          title="Reset to parent (inherit)"
          className="absolute right-2 top-7 text-text-muted hover:text-brand-400 transition-colors"
          aria-label="Reset to parent"
        >
          <RotateCcw size={11} />
        </button>
      )}
    </div>
  );
}
