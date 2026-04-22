import { useState, useEffect } from 'react';
import { Zap, X, Plus, Save } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { useQuickWeights, useSaveQuickWeights } from '../../hooks/useQuickWeights';

const MAX_ENTRIES = 6;

// Shortcut weights shown as quick-pick buttons on the console's new-spool
// wizard ("Full spool" step). Users can customise the list to whatever
// spool sizes they buy. Stored on the device in NVS.
export function QuickWeightsSection() {
  const { data } = useQuickWeights();
  const save = useSaveQuickWeights();

  const [grams, setGrams] = useState<number[]>([]);
  const [input, setInput] = useState('');

  useEffect(() => {
    if (data?.grams) setGrams(data.grams);
  }, [data]);

  const dirty = JSON.stringify(grams) !== JSON.stringify(data?.grams ?? []);
  const canAdd = grams.length < MAX_ENTRIES && parseInt(input, 10) > 0;

  const add = () => {
    const n = parseInt(input, 10);
    if (!canAdd) return;
    setGrams([...grams, n]);
    setInput('');
  };
  const removeAt = (idx: number) => setGrams(grams.filter((_, i) => i !== idx));

  return (
    <SectionCard
      title="Quick Weights"
      icon={<Zap size={16} />}
      description="Shortcut buttons for the console's new-spool wizard 'Full spool' step. Show one per common filament size you buy."
    >
      <div className="flex flex-wrap items-center gap-2">
        {grams.length === 0 ? (
          <div className="text-sm text-text-muted italic">No shortcuts set — defaults to 1000 / 2000 / 5000 g.</div>
        ) : (
          grams.map((g, i) => (
            <span
              key={i}
              className="inline-flex items-center gap-1.5 rounded-button bg-surface-input border border-surface-border px-3 py-1.5 font-mono text-sm text-text-primary"
            >
              {g} g
              <button
                onClick={() => removeAt(i)}
                className="text-text-muted hover:text-red-400 transition-colors"
                aria-label={`Remove ${g}g`}
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
          min={1}
          max={10000}
          className="w-32 bg-surface-input border border-surface-border rounded-input px-2 py-1 font-mono text-sm text-text-primary"
        />
        <Button variant="secondary" onClick={add} disabled={!canAdd}>
          <Plus size={14} className="mr-1 inline" /> Add
        </Button>
        <span className="text-xs text-text-muted">
          {grams.length}/{MAX_ENTRIES} used
        </span>
      </div>

      <div className="pt-2 flex items-center gap-3">
        <Button
          onClick={() => save.mutate({ grams })}
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
