import { useMemo, useState } from 'react';
import { Plus, Trash2, BookOpen, X, RotateCcw } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import { InputField } from '@spoolhard/ui/components/InputField';
import {
  useUpsertUserFilament,
  useCloudFilamentByName, useCloudFilamentById,
  cloudBodyToFilamentEntry,
  resolveUserFilament,
  type UserFilament,
  type UserFilamentVariant,
} from '../../hooks/useUserFilaments';
import { useFilamentsDb, type FilamentEntry } from '../../hooks/useFilamentsDb';
import { FilamentPicker } from '../spools/FilamentPicker';

// Known printer model codes that show up in the variants table dropdown.
// Free-text is allowed too — Bambu's compatible_printers strings are the
// source of truth, this list just keeps the typical case one-click.
const KNOWN_PRINTER_MODELS = ['X1C', 'X1', 'P1S', 'P1P', 'A1', 'A1mini', 'H2D', 'H2S'];
const COMMON_NOZZLE_DIAMETERS = [0.2, 0.4, 0.6, 0.8];

// Create/edit form for a user filament preset. Identity + operational
// range live at the top; per-(printer, nozzle) variants live in the
// table below. At spool-load time the firmware picks the variant
// matching the active printer's model code + nozzle diameter.
export function UserFilamentForm({
  initial, onClose,
}: {
  initial?: UserFilament;
  onClose: () => void;
}) {
  const upsert  = useUpsertUserFilament();
  const stockDb = useFilamentsDb();

  const [form, setForm] = useState<Partial<UserFilament>>({
    setting_id:        initial?.setting_id,
    name:              initial?.name ?? '',
    base_id:           initial?.base_id ?? '',
    parent_setting_id: initial?.parent_setting_id ?? '',
    cloud_inherits:    initial?.cloud_inherits ?? '',
    filament_type:     initial?.filament_type ?? '',
    filament_subtype:  initial?.filament_subtype ?? '',
    filament_vendor:   initial?.filament_vendor ?? '',
    filament_id:       initial?.filament_id ?? '',
    nozzle_temp_min:   initial?.nozzle_temp_min,
    nozzle_temp_max:   initial?.nozzle_temp_max,
    density:           initial?.density,
    variants:          initial?.variants ?? [],
  });
  const [pickerOpen, setPickerOpen] = useState(false);

  const set = <K extends keyof UserFilament>(k: K, v: UserFilament[K] | undefined) =>
    setForm((f) => ({ ...f, [k]: v }));

  const stockParent: FilamentEntry | null = useMemo(() => {
    const id = form.parent_setting_id;
    if (!id || !stockDb.data?.entries) return null;
    return stockDb.data.entries.find((e) => e.setting_id === id) ?? null;
  }, [form.parent_setting_id, stockDb.data]);

  const cloudInherits = form.cloud_inherits ?? '';
  const cloudByName = useCloudFilamentByName(
    cloudInherits, !!cloudInherits && !stockParent,
  );
  const baseId = form.base_id ?? '';
  const useBaseIdFallback = !stockParent && !cloudInherits && !!baseId;
  const cloudById = useCloudFilamentById(baseId, useBaseIdFallback);
  const cloudParent: FilamentEntry | null = useMemo(() => {
    if (cloudByName.data?.status === 'ok') return cloudBodyToFilamentEntry(cloudByName.data.body);
    if (cloudById.data?.status   === 'ok') return cloudBodyToFilamentEntry(cloudById.data.body);
    return null;
  }, [cloudByName.data, cloudById.data]);
  const parent = stockParent ?? cloudParent;

  const resolved = useMemo(
    () => resolveUserFilament(form as UserFilament, parent),
    [form, parent],
  );

  const applyStockBase = (e: FilamentEntry) => {
    setForm((f) => ({
      ...f,
      parent_setting_id: e.setting_id ?? '',
      name:              f.name && f.name.trim() ? f.name : `${e.name} (custom)`,
      base_id:           e.base_id        || f.base_id,
      filament_type:     e.material       || f.filament_type,
      filament_subtype:  e.subtype        || f.filament_subtype,
      filament_vendor:   e.brand          || f.filament_vendor,
      filament_id:       e.filament_id    || f.filament_id,
    }));
  };

  // Inheritable identity / range fields.
  const numericInput = (key: 'nozzle_temp_min' | 'nozzle_temp_max' | 'density') => {
    const current = form[key] as number | undefined;
    const isFloat = key === 'density';
    const isUnset = current === undefined || (isFloat ? current <= 0 : current < 0);
    const inherited = resolved.inherited[key];
    const placeholderValue = inherited ? resolved[key] : null;
    return {
      value: isUnset ? '' : String(current),
      placeholder: placeholderValue !== null && placeholderValue > 0
        ? (parent ? `${placeholderValue} (from ${parent.name})` : String(placeholderValue))
        : undefined,
      onChange: (ev: React.ChangeEvent<HTMLInputElement>) => {
        if (ev.target.value === '') set(key, undefined);
        else set(key, Number(ev.target.value));
      },
      onReset: () => set(key, undefined),
      canReset: !isUnset && !!parent,
      inherited,
    };
  };

  // Variant-table mutation helpers.
  const variants = form.variants ?? [];
  const updateVariant = (idx: number, patch: Partial<UserFilamentVariant>) => {
    const next = variants.map((v, i) => (i === idx ? { ...v, ...patch } : v));
    set('variants', next);
  };
  const addVariant = () => set('variants', [...variants, {
    printer_model:    KNOWN_PRINTER_MODELS[0],
    nozzle_diameter:  0.4,
    nozzle_temp_print:         resolved.nozzle_temp_max > 0 ? resolved.nozzle_temp_max : undefined,
    nozzle_temp_initial_layer: resolved.nozzle_temp_min > 0 ? resolved.nozzle_temp_min : undefined,
    extruder_variants:    ['Direct Drive Standard'],
    max_volumetric_speed: [0],
    pressure_advance:     [0],
  }]);
  const delVariant = (idx: number) => set('variants', variants.filter((_, i) => i !== idx));

  const onSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (!form.name) return;
    const body: Partial<UserFilament> = {
      ...form,
      nozzle_temp_min: form.nozzle_temp_min ?? -1,
      nozzle_temp_max: form.nozzle_temp_max ?? -1,
      density:         form.density         ?? 0,
      variants:        form.variants ?? [],
      setting_id:      initial?.setting_id,
    };
    upsert.mutate(body, { onSuccess: onClose });
  };

  const nMin = numericInput('nozzle_temp_min');
  const nMax = numericInput('nozzle_temp_max');
  const dens = numericInput('density');

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
      </div>

      <div className="border-t border-surface-border pt-3 space-y-3">
        <div className="flex items-center justify-between">
          <div>
            <div className="text-xs font-semibold uppercase tracking-wider text-text-secondary">
              Per-printer / nozzle variants
            </div>
            <div className="text-[11px] text-text-muted">
              Settings that change per (printer, nozzle): print temperature, max volumetric speed,
              and pressure-advance K. The console picks the matching row at spool-load time.
            </div>
          </div>
          <Button variant="secondary" type="button" onClick={addVariant} className="!py-1 !px-2">
            <Plus size={12} className="mr-1 inline" />
            <span className="text-xs">Add variant</span>
          </Button>
        </div>
        {variants.length === 0 ? (
          <div className="rounded-md border border-dashed border-surface-border px-3 py-4 text-[11px] text-text-muted italic text-center">
            No variants yet — add one per printer/nozzle pair you want characterised.
          </div>
        ) : (
          <div className="space-y-2">
            {variants.map((v, i) => (
              <VariantSection
                key={i}
                idx={i}
                variant={v}
                onChange={(patch) => updateVariant(i, patch)}
                onRemove={() => delVariant(i)}
              />
            ))}
          </div>
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

function fieldLabel(text: string, inherited: boolean): string {
  return inherited ? `${text} · inherited` : text;
}

interface NumericHelper {
  value: string;
  placeholder?: string;
  onChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
  onReset: () => void;
  canReset: boolean;
  inherited: boolean;
}
// One per-(printer, nozzle) variant rendered as a card. Header has the
// printer/nozzle selectors and a remove button. Body has print
// temperatures (scalar — Bambu doesn't vary them by extruder), then a
// per-extruder table where each row carries one entry from
// `extruder_variants` plus its parallel `max_volumetric_speed[i]` and
// `pressure_advance[i]`. The user can rename a row, change the values,
// and add/remove rows to match Bambu's `filament_extruder_variant`
// shape.
function VariantSection({
  idx, variant, onChange, onRemove,
}: {
  idx: number;
  variant: UserFilamentVariant;
  onChange: (patch: Partial<UserFilamentVariant>) => void;
  onRemove: () => void;
}) {
  const headerLabel = variant.printer_model || 'any printer';
  const nozzleLabel = variant.nozzle_diameter > 0 ? `${variant.nozzle_diameter} mm` : 'any nozzle';
  const labels = variant.extruder_variants ?? [];
  const speeds = variant.max_volumetric_speed ?? [];
  const ks     = variant.pressure_advance ?? [];
  const slotCount = Math.max(labels.length, speeds.length, ks.length);

  const updateSlot = (slotIdx: number, patch: { label?: string; speed?: number | undefined; k?: number | undefined }) => {
    const ev = [...labels];
    const mv = [...speeds];
    const pa = [...ks];
    while (ev.length <= slotIdx) ev.push('');
    while (mv.length <= slotIdx) mv.push(0);
    while (pa.length <= slotIdx) pa.push(0);
    if (patch.label !== undefined) ev[slotIdx] = patch.label;
    if (patch.speed !== undefined) mv[slotIdx] = patch.speed;
    if (patch.k     !== undefined) pa[slotIdx] = patch.k;
    onChange({ extruder_variants: ev, max_volumetric_speed: mv, pressure_advance: pa });
  };
  const addSlot = () => {
    onChange({
      extruder_variants:    [...labels, labels.length === 0 ? 'Direct Drive Standard' : 'Direct Drive High Flow'],
      max_volumetric_speed: [...speeds, 0],
      pressure_advance:     [...ks, 0],
    });
  };
  const delSlot = (slotIdx: number) => {
    onChange({
      extruder_variants:    labels.filter((_, i) => i !== slotIdx),
      max_volumetric_speed: speeds.filter((_, i) => i !== slotIdx),
      pressure_advance:     ks.filter((_, i) => i !== slotIdx),
    });
  };

  return (
    <div className="rounded-md border border-surface-border bg-surface-card/30">
      <div className="flex items-center gap-2 px-3 py-2 border-b border-surface-border">
        <span className="text-[10px] uppercase tracking-wider text-text-muted">Variant</span>
        <span className="text-xs font-mono text-text-primary">{headerLabel}</span>
        <span className="text-text-muted">·</span>
        <span className="text-xs font-mono text-text-primary">{nozzleLabel}</span>
        <div className="flex-1" />
        <button
          type="button"
          onClick={onRemove}
          className="text-text-muted hover:text-red-400 transition-colors"
          aria-label="Remove variant"
        >
          <Trash2 size={12} />
        </button>
      </div>
      <div className="space-y-3 p-3">
        <div className="grid grid-cols-2 gap-3">
          <div>
            <div className="text-[10px] uppercase tracking-wider text-text-muted mb-1">Printer model</div>
            <input
              list={`printer-models-${idx}`}
              value={variant.printer_model}
              onChange={(ev) => onChange({ printer_model: ev.target.value })}
              placeholder="any (wildcard)"
              className="w-full bg-surface-input border border-surface-border rounded px-2 py-1.5 font-mono text-xs text-text-primary"
            />
            <datalist id={`printer-models-${idx}`}>
              {KNOWN_PRINTER_MODELS.map((m) => <option key={m} value={m} />)}
            </datalist>
          </div>
          <div>
            <div className="text-[10px] uppercase tracking-wider text-text-muted mb-1">Nozzle diameter (mm)</div>
            <input
              list={`nozzles-${idx}`}
              type="number"
              step={0.1}
              value={variant.nozzle_diameter || ''}
              onChange={(ev) => onChange({ nozzle_diameter: Number(ev.target.value) })}
              placeholder="any (wildcard)"
              className="w-full bg-surface-input border border-surface-border rounded px-2 py-1.5 font-mono text-xs text-text-primary"
            />
            <datalist id={`nozzles-${idx}`}>
              {COMMON_NOZZLE_DIAMETERS.map((d) => <option key={d} value={d} />)}
            </datalist>
          </div>
          <div>
            <div className="text-[10px] uppercase tracking-wider text-text-muted mb-1">Print temp (°C)</div>
            <input
              type="number"
              value={variant.nozzle_temp_print ?? ''}
              onChange={(ev) => onChange({
                nozzle_temp_print: ev.target.value === '' ? undefined : Number(ev.target.value),
              })}
              className="w-full bg-surface-input border border-surface-border rounded px-2 py-1.5 font-mono text-xs text-text-primary"
            />
          </div>
          <div>
            <div className="text-[10px] uppercase tracking-wider text-text-muted mb-1">Initial-layer T (°C)</div>
            <input
              type="number"
              value={variant.nozzle_temp_initial_layer ?? ''}
              onChange={(ev) => onChange({
                nozzle_temp_initial_layer: ev.target.value === '' ? undefined : Number(ev.target.value),
              })}
              className="w-full bg-surface-input border border-surface-border rounded px-2 py-1.5 font-mono text-xs text-text-primary"
            />
          </div>
        </div>
        <div className="border-t border-surface-border pt-2">
          <div className="flex items-center justify-between mb-1">
            <div className="text-[10px] uppercase tracking-wider text-text-muted">
              Per-extruder values
            </div>
            <button
              type="button"
              onClick={addSlot}
              className="text-[11px] text-brand-400 hover:text-brand-300"
            >
              + add extruder
            </button>
          </div>
          {slotCount === 0 ? (
            <div className="rounded-md border border-dashed border-surface-border bg-surface-input/50 p-3 space-y-2">
              <div className="text-[11px] text-text-muted">
                No extruder values yet. Bambu's cloud preset may have left them implicit (inherited from
                the parent profile). Add the extruder type(s) below — common patterns are
                Direct Drive Standard alone, or Standard plus High Flow on H2S/H2D.
              </div>
              <div className="flex flex-wrap gap-2">
                <button
                  type="button"
                  onClick={() => onChange({
                    extruder_variants:    ['Direct Drive Standard'],
                    max_volumetric_speed: [0],
                    pressure_advance:     [0],
                  })}
                  className="text-[11px] rounded border border-surface-border px-2 py-1 hover:bg-surface-card"
                >
                  + Direct Drive Standard
                </button>
                <button
                  type="button"
                  onClick={() => onChange({
                    extruder_variants:    ['Direct Drive Standard', 'Direct Drive High Flow'],
                    max_volumetric_speed: [0, 0],
                    pressure_advance:     [0, 0],
                  })}
                  className="text-[11px] rounded border border-surface-border px-2 py-1 hover:bg-surface-card"
                >
                  + Standard + High Flow
                </button>
              </div>
            </div>
          ) : (
            <table className="w-full text-xs">
              <thead className="text-text-muted">
                <tr className="text-left">
                  <th className="font-normal py-1 pr-2">Extruder type</th>
                  <th className="font-normal py-1 pr-2">Max vol. (mm³/s)</th>
                  <th className="font-normal py-1 pr-2">PA (K)</th>
                  <th className="font-normal py-1 text-right"></th>
                </tr>
              </thead>
              <tbody>
                {Array.from({ length: slotCount }).map((_, i) => (
                  <tr key={i} className="border-t border-surface-border/40">
                    <td className="py-1 pr-2">
                      <input
                        list={`extruders-${idx}`}
                        value={labels[i] ?? ''}
                        onChange={(ev) => updateSlot(i, { label: ev.target.value })}
                        className="w-full bg-surface-input border border-surface-border rounded px-2 py-1 font-mono text-xs text-text-primary"
                      />
                    </td>
                    <td className="py-1 pr-2 w-32">
                      <input
                        type="number"
                        step={0.1}
                        value={speeds[i] ? speeds[i] : ''}
                        onChange={(ev) => updateSlot(i, {
                          speed: ev.target.value === '' ? 0 : Number(ev.target.value),
                        })}
                        className="w-full bg-surface-input border border-surface-border rounded px-2 py-1 font-mono text-xs text-text-primary"
                      />
                    </td>
                    <td className="py-1 pr-2 w-32">
                      <input
                        type="number"
                        step={0.001}
                        value={ks[i] ? ks[i] : ''}
                        onChange={(ev) => updateSlot(i, {
                          k: ev.target.value === '' ? 0 : Number(ev.target.value),
                        })}
                        className="w-full bg-surface-input border border-surface-border rounded px-2 py-1 font-mono text-xs text-text-primary"
                      />
                    </td>
                    <td className="py-1 text-right">
                      <button
                        type="button"
                        onClick={() => delSlot(i)}
                        className="text-text-muted hover:text-red-400 transition-colors"
                        aria-label="Remove extruder slot"
                      >
                        <Trash2 size={11} />
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
          <datalist id={`extruders-${idx}`}>
            <option value="Direct Drive Standard" />
            <option value="Direct Drive High Flow" />
          </datalist>
        </div>
      </div>
    </div>
  );
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
