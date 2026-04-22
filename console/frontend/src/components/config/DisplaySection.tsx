import { useState, useEffect } from 'react';
import { Monitor } from 'lucide-react';
import { useDisplayConfig, useSaveDisplayConfig } from '../../hooks/useDisplayConfig';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';

// Presets in seconds. 0 = never. Keep the list short — anything longer than
// 15 min is effectively "never" anyway and clutters the menu.
const PRESETS: { label: string; seconds: number }[] = [
  { label: 'Never',   seconds: 0    },
  { label: '30 sec',  seconds: 30   },
  { label: '1 min',   seconds: 60   },
  { label: '2 min',   seconds: 120  },
  { label: '5 min',   seconds: 300  },
  { label: '15 min',  seconds: 900  },
];

export function DisplaySection() {
  const { data } = useDisplayConfig();
  const save = useSaveDisplayConfig();
  const [seconds, setSeconds] = useState<number>(120);

  useEffect(() => {
    if (typeof data?.sleep_timeout_s === 'number') setSeconds(data.sleep_timeout_s);
  }, [data]);

  const dirty = data && data.sleep_timeout_s !== seconds;

  return (
    <SectionCard
      title="Display"
      icon={<Monitor size={16} />}
      description="Turn the touchscreen off after a period of inactivity to save the backlight. The next tap wakes it without activating any control."
    >
      <div className="flex flex-wrap gap-2">
        {PRESETS.map((p) => (
          <button
            key={p.seconds}
            onClick={() => setSeconds(p.seconds)}
            className={`px-3 py-1.5 rounded-button text-sm font-medium transition-colors ${
              seconds === p.seconds
                ? 'bg-brand-500 text-surface-body'
                : 'bg-surface-input text-text-secondary hover:bg-surface-card-hover border border-surface-border'
            }`}
          >
            {p.label}
          </button>
        ))}
      </div>

      <div className="flex items-center gap-3 pt-1">
        <label className="text-xs text-text-secondary">Custom (seconds):</label>
        <input
          type="number"
          min={0}
          max={3600}
          value={seconds}
          onChange={(e) => setSeconds(Math.max(0, Math.min(3600, parseInt(e.target.value, 10) || 0)))}
          className="w-24 bg-surface-input border border-surface-border rounded-input px-2 py-1 font-mono text-sm text-text-primary"
        />
        <span className="text-[11px] text-text-muted">0 = always on · max 3600</span>
      </div>

      <div className="flex items-center gap-3 pt-1">
        <Button
          onClick={() => save.mutate({ sleep_timeout_s: seconds })}
          disabled={save.isPending || !dirty}
        >
          {save.isPending ? 'Saving…' : 'Save'}
        </Button>
        {save.error instanceof Error && (
          <span className="text-xs text-status-error">{save.error.message}</span>
        )}
      </div>
    </SectionCard>
  );
}
