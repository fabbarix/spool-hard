import { useState, useEffect } from 'react';
import { Lightbulb, Play } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { useLedLegend, useLedTest, type LedState } from '../../hooks/useLedLegend';

const TEST_MS = 5000;

function Swatch({ s, testing }: { s: LedState; testing: boolean }) {
  const style: React.CSSProperties = { background: s.color };
  if (s.kind === 'flash' && s.period_ms) {
    style.animation = `led-flash ${s.period_ms}ms steps(2, end) infinite`;
  } else if (s.kind === 'pulse' && s.period_ms) {
    style.animation = `led-pulse ${s.period_ms}ms ease-in-out infinite`;
  } else if (s.kind === 'burst') {
    style.animation = 'led-burst 900ms steps(6, end) infinite';
  }
  return (
    <div className="relative h-8 w-8 shrink-0">
      <div
        className="h-full w-full rounded-full shadow-[inset_0_0_4px_rgba(0,0,0,0.4)]"
        style={style}
      />
      {testing && (
        <div className="absolute -inset-1 rounded-full border-2 border-brand-400 animate-pulse" />
      )}
    </div>
  );
}

export function LedLegendSection() {
  const legend = useLedLegend();
  const test = useLedTest();
  const [activeId, setActiveId] = useState<string | null>(null);

  // Auto-clear the "testing" highlight once the firmware's hold window
  // elapses. Slightly longer than TEST_MS so the indicator stays put
  // until the physical LED has actually returned to its prior state.
  useEffect(() => {
    if (!activeId) return;
    const t = setTimeout(() => setActiveId(null), TEST_MS + 250);
    return () => clearTimeout(t);
  }, [activeId]);

  const runTest = (id: string) => {
    setActiveId(id);
    test.mutate({ id, ms: TEST_MS });
  };

  return (
    <SectionCard
      title="LED Legend"
      icon={<Lightbulb size={16} />}
      description="What the on-device RGB LED is telling you. Press Test to drive each pattern for 5 seconds — useful for verifying the LED itself and for learning what each colour means."
    >
      {legend.isLoading && (
        <div className="text-sm text-text-secondary">Loading legend…</div>
      )}
      {legend.isError && (
        <div className="text-sm text-status-error">
          Could not load LED legend.
        </div>
      )}
      {legend.data && (
        <ul className="divide-y divide-surface-border">
          {legend.data.states.map((s) => (
            <li key={s.id} className="flex items-center gap-3 py-2.5">
              <Swatch s={s} testing={activeId === s.id} />
              <div className="flex-1 min-w-0">
                <div className="text-sm font-medium text-text-primary">
                  {s.label}
                </div>
                <div className="text-xs text-text-secondary mt-0.5">
                  {s.desc}
                </div>
              </div>
              <Button
                variant="secondary"
                className="shrink-0"
                onClick={() => runTest(s.id)}
                disabled={test.isPending && activeId === s.id}
              >
                <Play size={12} className="mr-1.5 inline" />
                {activeId === s.id ? 'Testing…' : 'Test'}
              </Button>
            </li>
          ))}
        </ul>
      )}
    </SectionCard>
  );
}
