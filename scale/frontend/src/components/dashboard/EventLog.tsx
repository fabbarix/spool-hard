import { useCallback, useEffect, useRef, useState } from 'react';
import { X, Activity } from 'lucide-react';
import type { LogEntry } from '../../types/websocket';
import { useWebSocket } from '@spoolhard/ui/providers/WebSocketProvider';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';

const clsColor: Record<LogEntry['cls'], string> = {
  evt: 'text-log-event',
  in: 'text-log-in',
  out: 'text-log-out',
  sys: 'text-log-sys',
  err: 'text-log-err',
};

export function EventLog() {
  const { subscribeLog } = useWebSocket();
  const [entries, setEntries] = useState<LogEntry[]>([]);
  const containerRef = useRef<HTMLDivElement>(null);

  const addEntry = useCallback((entry: LogEntry) => {
    setEntries((prev) => {
      const next = [...prev, entry];
      return next.length > 500 ? next.slice(-500) : next;
    });
  }, []);

  useEffect(() => {
    return subscribeLog('event', addEntry);
  }, [subscribeLog, addEntry]);

  useEffect(() => {
    const el = containerRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [entries]);

  return (
    <Card
      title="Event log"
      accentColor="#2dd4bf"
      actions={
        <Button variant="secondary" onClick={() => setEntries([])} className="px-2 py-1">
          <X size={14} />
        </Button>
      }
    >
      <div ref={containerRef} className="h-64 overflow-y-auto font-mono text-xs leading-5 log-scroll">
        {entries.map((e, i) => (
          <div key={i} className={clsColor[e.cls]}>
            <span className="text-text-muted">{e.ts}</span> {e.text}
          </div>
        ))}
        {entries.length === 0 && (
          <div className="flex flex-col items-center justify-center h-full text-text-muted">
            <Activity size={24} className="mb-2" />
            <span className="text-sm">Waiting for events...</span>
          </div>
        )}
      </div>
    </Card>
  );
}
