import { useState } from 'react';
import { Scale, Trash2, Plus, Save, X, Pencil } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { InputField } from '@spoolhard/ui/components/InputField';
import {
  useCoreWeights,
  useUpsertCoreWeight,
  useDeleteCoreWeight,
  coreKey,
  type CoreWeightEntry,
} from '../../hooks/useCoreWeights';

// Editable table of (brand, material, advertised) → empty-spool core weight.
// Populated automatically by the on-device wizard when a user registers a
// full/empty spool; can also be edited by hand here. The wizard's "Used
// spool → Pick from DB" step consumes this table, and the spool-screen
// "Capture weight" button subtracts these values from the scale reading to
// store filament-only weight on the matching spool record.
export function CoreWeightsSection() {
  const { data, isLoading } = useCoreWeights();
  const upsert = useUpsertCoreWeight();
  const del    = useDeleteCoreWeight();
  const [adding, setAdding] = useState(false);

  return (
    <SectionCard
      title="Core Weights"
      icon={<Scale size={16} />}
      description="Empty-spool weights learned by the console when you register a new spool. Consumed by the new-spool wizard's 'Used spool → Pick from DB' step and by the spool screen's 'Capture weight' button (subtracted from the scale reading)."
    >
      {isLoading ? (
        <div className="text-sm text-text-muted">Loading…</div>
      ) : !data || data.length === 0 ? (
        <div className="text-sm text-text-muted italic py-4 text-center">
          No core weights recorded yet. Register a full or empty spool on the console, or add one manually below.
        </div>
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-sm tabular-nums">
            <thead className="text-xs text-text-muted">
              <tr className="text-left">
                <th className="font-normal py-1 pr-3">Brand</th>
                <th className="font-normal py-1 pr-3">Material</th>
                <th className="font-normal py-1 pr-3">Advertised</th>
                <th className="font-normal py-1 pr-3">Core</th>
                <th className="font-normal py-1 pr-3 text-right">Actions</th>
              </tr>
            </thead>
            <tbody>
              {data.map((e) => (
                <Row
                  key={coreKey(e)}
                  entry={e}
                  onDelete={() => del.mutate(coreKey(e))}
                  upsert={upsert}
                />
              ))}
            </tbody>
          </table>
        </div>
      )}

      <div className="pt-2">
        {adding ? (
          <AddRow onCancel={() => setAdding(false)} onSaved={() => setAdding(false)} upsert={upsert} />
        ) : (
          <Button variant="secondary" onClick={() => setAdding(true)}>
            <Plus size={14} className="mr-1 inline" /> Add entry
          </Button>
        )}
      </div>
    </SectionCard>
  );
}

function Row({ entry, onDelete, upsert }: {
  entry: CoreWeightEntry;
  onDelete: () => void;
  upsert: ReturnType<typeof useUpsertCoreWeight>;
}) {
  const [editing, setEditing] = useState(false);
  const [grams, setGrams] = useState(String(entry.grams));
  const canSave = parseInt(grams, 10) >= 0;

  // Only the `grams` cell is editable — the (brand, material, advertised)
  // triple is the composite key, so changing any of those would conceptually
  // be a delete + re-add. Users who need that can just delete the row and
  // add a new one.
  const save = () => {
    if (!canSave) return;
    upsert.mutate(
      { brand: entry.brand, material: entry.material, advertised: entry.advertised, grams: parseInt(grams, 10) },
      { onSuccess: () => setEditing(false) }
    );
  };

  const cancel = () => {
    setGrams(String(entry.grams));
    setEditing(false);
  };

  return (
    <tr className="border-t border-surface-border/50">
      <td className="py-1.5 pr-3 text-text-primary">{entry.brand}</td>
      <td className="py-1.5 pr-3 text-text-primary">{entry.material}</td>
      <td className="py-1.5 pr-3 text-text-muted">{entry.advertised} g</td>
      <td className="py-1.5 pr-3 text-brand-400 font-mono">
        {editing ? (
          <input
            type="number"
            value={grams}
            onChange={(e) => setGrams(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') save(); if (e.key === 'Escape') cancel(); }}
            autoFocus
            className="w-20 bg-surface-input border border-surface-border rounded px-2 py-0.5 text-brand-400 font-mono text-sm"
          />
        ) : (
          <span>{entry.grams} g</span>
        )}
      </td>
      <td className="py-1.5 pr-3 text-right">
        {editing ? (
          <span className="inline-flex items-center gap-1">
            <button
              onClick={save}
              disabled={!canSave || upsert.isPending}
              className="text-text-muted hover:text-brand-400 transition-colors disabled:opacity-40"
              aria-label="Save"
            >
              <Save size={14} />
            </button>
            <button
              onClick={cancel}
              className="text-text-muted hover:text-text-primary transition-colors"
              aria-label="Cancel"
            >
              <X size={14} />
            </button>
          </span>
        ) : (
          <span className="inline-flex items-center gap-2">
            <button
              onClick={() => setEditing(true)}
              className="text-text-muted hover:text-brand-400 transition-colors"
              aria-label="Edit core weight"
            >
              <Pencil size={14} />
            </button>
            <button
              onClick={onDelete}
              className="text-text-muted hover:text-red-400 transition-colors"
              aria-label="Delete row"
            >
              <Trash2 size={14} />
            </button>
          </span>
        )}
      </td>
    </tr>
  );
}

function AddRow({ onCancel, onSaved, upsert }: {
  onCancel: () => void;
  onSaved: () => void;
  upsert: ReturnType<typeof useUpsertCoreWeight>;
}) {
  const [brand, setBrand]       = useState('');
  const [material, setMaterial] = useState('');
  const [advertised, setAdv]    = useState('');
  const [grams, setGrams]       = useState('');
  const canSave = brand.trim() && material.trim() && parseInt(advertised, 10) > 0 && parseInt(grams, 10) >= 0;

  const save = () => {
    if (!canSave) return;
    upsert.mutate(
      {
        brand: brand.trim(),
        material: material.trim(),
        advertised: parseInt(advertised, 10),
        grams: parseInt(grams, 10),
      },
      { onSuccess: () => onSaved() }
    );
  };

  return (
    <div className="grid grid-cols-1 md:grid-cols-5 gap-2 pt-2 border-t border-surface-border">
      <InputField label="Brand"      value={brand}      onChange={(e) => setBrand(e.target.value)} />
      <InputField label="Material"   value={material}   onChange={(e) => setMaterial(e.target.value)} />
      <InputField label="Advertised (g)" type="number" value={advertised} onChange={(e) => setAdv(e.target.value)} />
      <InputField label="Core (g)"       type="number" value={grams}      onChange={(e) => setGrams(e.target.value)} />
      <div className="flex items-end gap-1.5">
        <Button onClick={save} disabled={!canSave || upsert.isPending}>
          <Save size={14} className="mr-1 inline" /> Save
        </Button>
        <Button variant="secondary" onClick={onCancel}>
          <X size={14} className="mr-1 inline" /> Cancel
        </Button>
      </div>
    </div>
  );
}
