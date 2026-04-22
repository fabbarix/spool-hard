import { useEffect, useState } from 'react';
import { Scale, Save, Gauge, Thermometer, StickyNote, Printer, BookOpen } from 'lucide-react';
import { Button } from '@spoolhard/ui/components/Button';
import { InputField } from '@spoolhard/ui/components/InputField';
import { useSpoolUpsert, type SpoolRecord } from '../../hooks/useSpools';
import { useScaleLink } from '../../hooks/useScaleLink';
import { FilamentPicker } from './FilamentPicker';
import type { FilamentEntry } from '../../hooks/useFilamentsDb';

interface Props {
  spool: SpoolRecord;
}

// Editable detail panel for a single spool. Mounted inline underneath the
// SpoolRow when expanded. Three groups of fields:
//   * Identification (material / subtype / brand / color code+name)
//   * Weights (advertised / core / new — all in grams, whole numbers)
//   * "Capture current weight" — reads the latest stable reading from the
//     paired scale and stamps it into weight_current, resetting the
//     consumed_since_weight counter. Disabled when the scale is offline or
//     the last reading isn't stable.
export function SpoolDetailPanel({ spool }: Props) {
  const upsert = useSpoolUpsert();
  const { data: scale } = useScaleLink();

  // Local draft so the user can edit without every keystroke round-tripping.
  const [draft, setDraft] = useState<SpoolRecord>(spool);
  useEffect(() => { setDraft(spool); }, [spool]);
  const [pickerOpen, setPickerOpen] = useState(false);

  // Overlay semantics: the library only exposes what the preset actually
  // knew (name-derived material/subtype/brand, plus resolved temps +
  // filament_id). Fields the library left undefined don't overwrite what
  // the user already typed — matches the firmware's partial-merge on save.
  const applyFilament = (e: FilamentEntry) => {
    setDraft((d) => ({
      ...d,
      material_type:    e.material    || d.material_type,
      material_subtype: e.subtype     || d.material_subtype,
      brand:            e.brand       || d.brand,
      color_code:       e.color_code  || d.color_code,
      nozzle_temp_min:  typeof e.nozzle_temp_min === 'number' ? e.nozzle_temp_min : d.nozzle_temp_min,
      nozzle_temp_max:  typeof e.nozzle_temp_max === 'number' ? e.nozzle_temp_max : d.nozzle_temp_max,
      density:          typeof e.density === 'number' && e.density > 0 ? e.density : d.density,
      slicer_filament:  e.filament_id || d.slicer_filament,
      weight_advertised: typeof e.advertised === 'number' && e.advertised > 0
        ? e.advertised
        : d.weight_advertised,
    }));
  };

  // Detect changes field-by-field so Save lights up only when something
  // actually differs. Comparing whole objects would be over-triggered by
  // React Query refetches re-seeding `spool`.
  const dirty =
    draft.material_type !== spool.material_type ||
    draft.material_subtype !== spool.material_subtype ||
    draft.brand !== spool.brand ||
    draft.color_name !== spool.color_name ||
    draft.color_code !== spool.color_code ||
    draft.weight_advertised !== spool.weight_advertised ||
    draft.weight_core !== spool.weight_core ||
    draft.weight_new !== spool.weight_new ||
    draft.nozzle_temp_min !== spool.nozzle_temp_min ||
    draft.nozzle_temp_max !== spool.nozzle_temp_max ||
    draft.slicer_filament !== spool.slicer_filament ||
    draft.note !== spool.note;

  const save = () => upsert.mutate(draft);

  const captureWeight = () => {
    if (!scale?.weight || scale.weight.state !== 'stable') return;
    const g = Math.round(scale.weight.grams);
    upsert.mutate({
      id: spool.id,
      weight_current: g,
      consumed_since_weight: 0,   // any subsequent consumption is new
    });
  };

  const weight = scale?.weight;
  const canCapture = scale?.connected && weight && weight.state === 'stable' && weight.grams > 0;

  // Hex color input: frontend stores color_code without the '#'. The input is
  // a text field (not <input type="color">) because we also want freeform
  // SpoolHard color names; the <input type="color"> picker is ugly and
  // doesn't respect our theme.
  const hexPreview = draft.color_code ? `#${draft.color_code.slice(0, 6)}` : 'transparent';

  return (
    <div className="border-t border-surface-border p-4 bg-surface-card/40 space-y-4">
      {/* Identification */}
      <div className="flex items-center justify-between">
        <span className="text-xs font-semibold uppercase tracking-wider text-text-secondary">Identification</span>
        <Button variant="secondary" onClick={() => setPickerOpen(true)} className="!py-1.5 !px-2.5">
          <BookOpen size={13} className="mr-1.5 inline" />
          <span className="text-xs">Load from library</span>
        </Button>
      </div>
      <div className="grid grid-cols-1 md:grid-cols-2 gap-3">
        <InputField
          label="Material"
          value={draft.material_type ?? ''}
          onChange={(e) => setDraft({ ...draft, material_type: e.target.value })}
        />
        <InputField
          label="Subtype"
          value={draft.material_subtype ?? ''}
          onChange={(e) => setDraft({ ...draft, material_subtype: e.target.value })}
        />
        <InputField
          label="Brand"
          value={draft.brand ?? ''}
          onChange={(e) => setDraft({ ...draft, brand: e.target.value })}
        />
        <InputField
          label="Color name"
          value={draft.color_name ?? ''}
          onChange={(e) => setDraft({ ...draft, color_name: e.target.value })}
        />
      </div>

      <div className="flex items-center gap-3">
        <label className="text-xs text-text-muted">Color</label>
        <input
          type="color"
          value={hexPreview === 'transparent' ? '#000000' : hexPreview}
          onChange={(e) => setDraft({ ...draft, color_code: e.target.value.replace(/^#/, '') })}
          className="w-10 h-10 rounded-md border border-surface-border cursor-pointer bg-transparent p-0.5"
        />
        {draft.color_code && (
          <span className="text-xs text-text-muted font-mono">#{draft.color_code.slice(0, 6)}</span>
        )}
      </div>

      {/* Weights */}
      <div className="grid grid-cols-2 md:grid-cols-3 gap-3">
        <InputField
          label="Advertised (g)"
          type="number"
          value={draft.weight_advertised ?? ''}
          onChange={(e) =>
            setDraft({ ...draft, weight_advertised: e.target.value === '' ? -1 : parseInt(e.target.value, 10) })
          }
        />
        <InputField
          label="Empty core (g)"
          type="number"
          value={draft.weight_core ?? ''}
          onChange={(e) =>
            setDraft({ ...draft, weight_core: e.target.value === '' ? -1 : parseInt(e.target.value, 10) })
          }
        />
        <InputField
          label="New weight (g)"
          type="number"
          value={draft.weight_new ?? ''}
          onChange={(e) =>
            setDraft({ ...draft, weight_new: e.target.value === '' ? -1 : parseInt(e.target.value, 10) })
          }
        />
      </div>

      {/* Print settings — pushed to Bambu via ams_filament_setting when this
          spool gets auto-assigned after a tag scan. Empty/unset temps fall
          back to a material-default lookup in the firmware. */}
      <div className="border-t border-surface-border pt-3 space-y-2">
        <div className="flex items-center gap-1.5 text-xs text-text-muted">
          <Thermometer size={12} /> Print settings <Printer size={11} className="ml-1 opacity-60" />
        </div>
        <div className="grid grid-cols-2 md:grid-cols-3 gap-3">
          <InputField
            label="Nozzle min (°C)"
            type="number"
            value={draft.nozzle_temp_min != null && draft.nozzle_temp_min >= 0 ? draft.nozzle_temp_min : ''}
            onChange={(e) =>
              setDraft({ ...draft, nozzle_temp_min: e.target.value === '' ? -1 : parseInt(e.target.value, 10) })
            }
          />
          <InputField
            label="Nozzle max (°C)"
            type="number"
            value={draft.nozzle_temp_max != null && draft.nozzle_temp_max >= 0 ? draft.nozzle_temp_max : ''}
            onChange={(e) =>
              setDraft({ ...draft, nozzle_temp_max: e.target.value === '' ? -1 : parseInt(e.target.value, 10) })
            }
          />
          <InputField
            label="Bambu filament ID"
            placeholder="e.g. GFL99"
            value={draft.slicer_filament ?? ''}
            onChange={(e) => setDraft({ ...draft, slicer_filament: e.target.value })}
          />
        </div>
      </div>

      {/* Freeform note. Mirrors yanshay/SpoolEase's SpoolRecord.note. */}
      <div className="border-t border-surface-border pt-3 space-y-2">
        <div className="flex items-center gap-1.5 text-xs text-text-muted">
          <StickyNote size={12} /> Note
        </div>
        <textarea
          value={draft.note ?? ''}
          onChange={(e) => setDraft({ ...draft, note: e.target.value })}
          placeholder="Anything worth remembering about this spool."
          rows={2}
          className="w-full bg-surface-input border border-surface-border rounded-md px-3 py-2 text-sm text-text-primary focus:outline-none focus:ring-1 focus:ring-brand-500"
        />
      </div>

      <div className="flex items-center justify-between pt-1">
        <Button onClick={save} disabled={!dirty || upsert.isPending}>
          <Save size={14} className="mr-1.5 inline" />
          {upsert.isPending ? 'Saving…' : 'Save'}
        </Button>
        {upsert.error instanceof Error && (
          <span className="text-xs text-status-error">{upsert.error.message}</span>
        )}
      </div>

      {/* Current weight capture */}
      <div className="border-t border-surface-border pt-3">
        <div className="flex items-center gap-1.5 text-xs text-text-muted mb-2">
          <Scale size={12} /> Current weight
        </div>
        <div className="flex items-center gap-3">
          <div className="font-mono tabular-nums">
            {typeof draft.weight_current === 'number' && draft.weight_current >= 0 ? (
              <span className="text-brand-400 text-lg">{draft.weight_current}g</span>
            ) : (
              <span className="text-text-muted">never weighed</span>
            )}
          </div>
          {typeof draft.consumed_since_weight === 'number' && draft.consumed_since_weight > 0 && (
            <div className="text-xs text-text-muted">
              − {draft.consumed_since_weight.toFixed(0)}g predicted since last weigh
            </div>
          )}
        </div>
        <div className="mt-2 flex items-center gap-3">
          <Button variant="secondary" onClick={captureWeight} disabled={!canCapture || upsert.isPending}>
            <Scale size={14} className="mr-1.5 inline" />
            Capture from scale
          </Button>
          <div className="text-xs text-text-muted font-mono">
            {weight
              ? weight.state
                ? `scale: ${weight.grams.toFixed(Math.max(0, Math.min(4, weight.precision ?? 0)))}g (${weight.state})`
                : 'scale: no reading yet'
              : 'scale: disconnected'}
          </div>
        </div>
      </div>

      {/* K-values */}
      {spool.ext?.k_values && spool.ext.k_values.length > 0 && (
        <div className="border-t border-surface-border pt-3">
          <div className="flex items-center gap-1.5 text-xs text-text-muted mb-2">
            <Gauge size={12} /> Pressure-advance K
          </div>
          <div className="overflow-x-auto">
            <table className="w-full text-xs font-mono tabular-nums">
              <thead className="text-text-muted">
                <tr className="text-left">
                  <th className="font-normal py-1 pr-4">Printer</th>
                  <th className="font-normal py-1 pr-4">Nozzle</th>
                  <th className="font-normal py-1 pr-4">Extruder</th>
                  <th className="font-normal py-1 pr-4">K</th>
                  <th className="font-normal py-1 pr-4">Cali idx</th>
                </tr>
              </thead>
              <tbody>
                {spool.ext.k_values.map((e, i) => (
                  <tr key={i} className="border-t border-surface-border/50">
                    <td className="py-1 pr-4 text-text-primary">{e.printer}</td>
                    <td className="py-1 pr-4">{e.nozzle.toFixed(1)} mm</td>
                    <td className="py-1 pr-4">{e.extruder}</td>
                    <td className="py-1 pr-4 text-brand-400">{e.k.toFixed(3)}</td>
                    <td className="py-1 pr-4">{e.cali_idx >= 0 ? e.cali_idx : '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {pickerOpen && (
        <FilamentPicker
          onPick={applyFilament}
          onClose={() => setPickerOpen(false)}
        />
      )}
    </div>
  );
}
