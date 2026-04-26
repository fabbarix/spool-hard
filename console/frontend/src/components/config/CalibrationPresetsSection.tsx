import { useState, useEffect, useMemo } from 'react';
import { Weight, X, Plus, Save } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { useCalibrationPresets, useSaveCalibrationPresets } from '../../hooks/useCalibrationPresets';

const MAX_ENTRIES = 12;

// Reference-weight presets shown on the console LCD's scale-calibration
// wizard. The user adds whatever physical weights they own — the LCD
// renders one chip per preset on step 1 of the wizard, and the user
// taps the matching chip when capturing a calibration point.
export function CalibrationPresetsSection() {
  const { data } = useCalibrationPresets();
  const save = useSaveCalibrationPresets();

  const [presets, setPresets] = useState<number[]>([]);
  const [input, setInput]     = useState('');

  useEffect(() => {
    if (data?.presets) setPresets(data.presets);
  }, [data]);

  const dirty = JSON.stringify(presets) !== JSON.stringify(data?.presets ?? []);

  // Validate + dedupe + sort BEFORE submission so the local list always
  // mirrors what the firmware would persist (the firmware re-normalises
  // server-side too, but echoing matches lets us show "no change" on a
  // duplicate-only edit).
  const inputN = parseInt(input, 10);
  const canAdd = Number.isFinite(inputN) && inputN > 0
              && presets.length < MAX_ENTRIES
              && !presets.includes(inputN);

  const add = () => {
    if (!canAdd) return;
    setPresets([...presets, inputN].sort((a, b) => a - b));
    setInput('');
  };
  const removeAt = (idx: number) => setPresets(presets.filter((_, i) => i !== idx));

  // Render kg for >=1000 g so big weights stay easy to read at a glance —
  // matches the LCD's chip labels.
  const fmt = useMemo(() => (g: number) => {
    if (g >= 1000 && g % 100 === 0) {
      return g % 1000 === 0 ? `${g / 1000} kg` : `${(g / 1000).toFixed(1)} kg`;
    }
    return `${g} g`;
  }, []);

  return (
    <SectionCard
      title="Calibration Weights"
      icon={<Weight size={16} />}
      description="Reference weights you have available. The console LCD's scale-calibration wizard renders one chip per preset on its 'pick a known weight' step."
    >
      <div className="flex flex-wrap items-center gap-2">
        {presets.length === 0 ? (
          <div className="text-sm text-text-muted italic">
            No presets — defaults to 100 / 250 / 500 g and 1 / 2 / 5 kg.
          </div>
        ) : (
          presets.map((g, i) => (
            <span
              key={g}
              className="inline-flex items-center gap-1.5 rounded-button bg-surface-input border border-surface-border px-3 py-1.5 font-mono text-sm text-text-primary"
            >
              {fmt(g)}
              <button
                onClick={() => removeAt(i)}
                className="text-text-muted hover:text-red-400 transition-colors"
                aria-label={`Remove ${fmt(g)}`}
              >
                <X size={12} />
              </button>
            </span>
          ))
        )}
      </div>

      <div className="flex items-center gap-2 pt-2">
        <input
          type="number"
          placeholder="grams"
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={(e) => { if (e.key === 'Enter') add(); }}
          min={1}
          max={50000}
          className="w-32 bg-surface-input border border-surface-border rounded-input px-2 py-1 font-mono text-sm text-text-primary"
        />
        <Button variant="secondary" onClick={add} disabled={!canAdd}>
          <Plus size={14} className="mr-1 inline" /> Add
        </Button>
        <span className="text-xs text-text-muted">
          {presets.length}/{MAX_ENTRIES} used
        </span>
      </div>

      <div className="pt-2 flex items-center gap-3">
        <Button
          onClick={() => save.mutate({ presets })}
          disabled={!dirty || save.isPending}
        >
          <Save size={14} className="mr-1 inline" />
          {save.isPending ? 'Saving…' : 'Save'}
        </Button>
        {save.error instanceof Error && (
          <span className="text-xs text-status-error">{save.error.message}</span>
        )}
      </div>
    </SectionCard>
  );
}
