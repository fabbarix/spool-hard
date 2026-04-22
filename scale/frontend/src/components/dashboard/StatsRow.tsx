import { useWebSocket } from '@spoolhard/ui/providers/WebSocketProvider';
import { StatusDot } from '@spoolhard/ui/components/StatusDot';
import { StatCard } from './StatCard';
import { useScaleConfig } from '../../hooks/useScaleConfig';

export function StatsRow() {
  const { rawSample, weightState, nfcEvent, consoleConn } = useWebSocket();
  const { data: cfg } = useScaleConfig();

  const precision = Math.max(0, Math.min(4, cfg?.precision ?? 1));
  const grams = rawSample?.weight_g;

  const nfcText = nfcEvent
    ? nfcEvent.uid
      ? nfcEvent.uid.map((b) => b.toString(16).padStart(2, '0')).join(':')
      : `status ${nfcEvent.status}`
    : '--';

  return (
    <div className="space-y-3">
      <StatCard
        label="Weight"
        dataColor={false}
        value={
          grams != null ? (
            <span className="flex items-baseline gap-3 min-w-0">
              <span className="text-2xl text-text-data truncate">
                {grams.toFixed(precision)} g
              </span>
              <span className="text-sm text-text-muted truncate">
                {grams.toFixed(4)} g
              </span>
            </span>
          ) : (
            <span className="text-2xl text-text-data">--</span>
          )
        }
        className="shadow-[0_0_30px_rgba(240,180,41,0.03)] overflow-hidden"
        style={{ animationDelay: '0ms' }}
      />
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
        <StatCard
          label="Raw"
          dataColor={false}
          value={
            <span className="block text-sm text-text-data truncate" title={rawSample ? String(rawSample.raw) : ''}>
              {rawSample ? String(rawSample.raw) : '--'}
            </span>
          }
          className="overflow-hidden"
          style={{ animationDelay: '50ms' }}
        />
        <StatCard
          label="Scale state"
          dataColor={false}
          value={
            <span className="block text-sm text-text-primary truncate" title={weightState?.state ?? ''}>
              {weightState?.state ?? '--'}
            </span>
          }
          className="overflow-hidden"
          style={{ animationDelay: '100ms' }}
        />
        <StatCard
          label="NFC"
          dataColor={false}
          value={
            <span className="block text-sm text-text-primary truncate font-mono" title={nfcText}>
              {nfcText}
            </span>
          }
          className="overflow-hidden"
          style={{ animationDelay: '150ms' }}
        />
        <StatCard
          label="Console"
          dataColor={false}
          value={
            <span className="flex items-center gap-2 min-w-0">
              <StatusDot status={consoleConn?.connected ? 'connected' : 'disconnected'} />
              <span className="text-sm truncate">{consoleConn?.ip ?? 'N/A'}</span>
            </span>
          }
          className="overflow-hidden"
          style={{ animationDelay: '200ms' }}
        />
      </div>
    </div>
  );
}
