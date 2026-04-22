import { useCallback, useEffect, useRef, useState } from 'react';
import { Pause, Play, X, Activity } from 'lucide-react';
import type { LogEntry } from '../../types/websocket';
import { useWebSocket } from '@spoolhard/ui/providers/WebSocketProvider';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';

const clsColor: Record<string, string> = {
  in: 'text-log-in',
  out: 'text-log-out',
  sys: 'text-log-sys',
  err: 'text-log-err',
  evt: 'text-log-event',
};

export function ConsoleLog() {
  const { subscribeLog } = useWebSocket();
  const [entries, setEntries] = useState<LogEntry[]>([]);
  const [paused, setPaused] = useState(false);
  const bufferRef = useRef<LogEntry[]>([]);
  const containerRef = useRef<HTMLDivElement>(null);
  const pausedRef = useRef(paused);
  pausedRef.current = paused;

  const addEntry = useCallback((entry: LogEntry) => {
    if (pausedRef.current) {
      bufferRef.current.push(entry);
    } else {
      setEntries((prev) => {
        const next = [...prev, entry];
        return next.length > 500 ? next.slice(-500) : next;
      });
    }
  }, []);

  useEffect(() => {
    return subscribeLog('console', addEntry);
  }, [subscribeLog, addEntry]);

  useEffect(() => {
    const el = containerRef.current;
    if (el && !paused) el.scrollTop = el.scrollHeight;
  }, [entries, paused]);

  function handleResume() {
    setEntries((prev) => {
      const merged = [...prev, ...bufferRef.current];
      bufferRef.current = [];
      return merged.length > 500 ? merged.slice(-500) : merged;
    });
    setPaused(false);
  }

  return (
    <Card
      title="Console log"
      accentColor="#f0b429"
      actions={
        <div className="flex items-center gap-2">
          {paused ? (
            <Button variant="secondary" onClick={handleResume} className="px-2 py-1 flex items-center gap-1">
              <Play size={14} />
              {bufferRef.current.length > 0 && (
                <span className="bg-brand-500/20 text-brand-400 text-[10px] px-1.5 rounded-full">
                  {bufferRef.current.length}
                </span>
              )}
            </Button>
          ) : (
            <Button variant="secondary" onClick={() => setPaused(true)} className="px-2 py-1">
              <Pause size={14} />
            </Button>
          )}
          <Button
            variant="secondary"
            className="px-2 py-1"
            onClick={() => {
              setEntries([]);
              bufferRef.current = [];
            }}
          >
            <X size={14} />
          </Button>
        </div>
      }
    >
      <div ref={containerRef} className="h-64 overflow-y-auto font-mono text-xs leading-5 log-scroll">
        {entries.map((e, i) => (
          <div key={i} className={clsColor[e.cls] ?? 'text-text-primary'}>
            <span className="text-text-muted">{e.ts}</span>{' '}
            {e.cls === 'out' ? '\u25B6' : '\u25C0'} {e.text}
          </div>
        ))}
        {entries.length === 0 && (
          <div className="flex flex-col items-center justify-center h-full text-text-muted">
            <Activity size={24} className="mb-2" />
            <span className="text-sm">No console traffic yet.</span>
          </div>
        )}
      </div>
    </Card>
  );
}
