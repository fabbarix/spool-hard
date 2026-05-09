import { StatusDot } from '@spoolhard/ui/components/StatusDot';
import { StatCard } from '@spoolhard/ui/components/StatCard';
import { formatUptime } from '@spoolhard/ui/utils/formatUptime';
import { useLiveUptime } from '@spoolhard/ui/utils/useLiveUptime';
import { useScaleLink } from '../hooks/useScaleLink';
import { useFirmwareInfo } from '../hooks/useFirmwareInfo';
import { useSpools } from '../hooks/useSpools';
import { PrintersPanel } from '../components/dashboard/PrintersPanel';

function fmtMB(bytes: number) { return (bytes / 1024 / 1024).toFixed(0); }
function fmtGB(bytes: number) { return (bytes / 1024 / 1024 / 1024).toFixed(1); }

// Tailwind class palette for the weight-state badge. Mirrors the same
// hue family the on-device LED uses: dark-teal for active load, muted
// for removed, red for uncalibrated.
function stateClass(s: string | undefined): string {
  switch (s) {
    case 'stable':       return 'bg-emerald-500/10 text-emerald-300 border-emerald-500/30';
    case 'new':          return 'bg-emerald-500/10 text-emerald-300 border-emerald-500/30';
    case 'unstable':     return 'bg-amber-500/10  text-amber-300  border-amber-500/30';
    case 'removed':      return 'bg-surface-input text-text-muted border-surface-border';
    case 'uncalibrated': return 'bg-red-500/10    text-red-300    border-red-500/30';
    default:             return 'bg-surface-input text-text-muted border-surface-border';
  }
}

export function DashboardPage() {
  // dataUpdatedAt lets useLiveUptime extrapolate between WS pushes so
  // the on-screen seconds tick up at 1 Hz instead of jumping every
  // 5 s when a fresh heartbeat lands.
  const { data: scale, dataUpdatedAt: scaleAt } = useScaleLink();
  const { data: fw,    dataUpdatedAt: fwAt    } = useFirmwareInfo();
  const { data: spools } = useSpools(0, 1);

  const consoleUptime = useLiveUptime(fw?.uptime_s, fwAt);
  const scaleUptime   = useLiveUptime(scale?.telemetry?.uptime_s, scaleAt);

  const w        = scale?.weight;
  const grams    = w?.grams;
  const wState   = w?.state ?? '';
  const precision = Math.max(0, Math.min(4, w?.precision ?? 0));
  // hasReading: there's a non-empty weight state and we've actually
  // received a reading (ago_ms === -1 means "never seen").
  const hasReading = scale?.connected && wState !== '' && (w?.ago_ms ?? -1) >= 0;

  return (
    <div className="space-y-4">
      {/* Metrics row */}
      <div className="grid grid-cols-2 sm:grid-cols-5 gap-3">
        <StatCard
          label="Spools"
          value={spools?.total ?? '--'}
          style={{ animationDelay: '0ms' }}
        />
        <StatCard
          label="Firmware"
          value={fw?.fw_version ?? '--'}
          dataColor={false}
          style={{ animationDelay: '50ms' }}
        />
        <StatCard
          label="Uptime"
          value={formatUptime(consoleUptime)}
          dataColor={false}
          style={{ animationDelay: '75ms' }}
        />
        <StatCard
          label="Heap"
          value={fw ? `${(fw.free_heap / 1024).toFixed(0)} K` : '--'}
          style={{ animationDelay: '100ms' }}
        />
        <StatCard
          label="PSRAM"
          value={fw ? `${(fw.psram_free / 1024).toFixed(0)} K` : '--'}
          style={{ animationDelay: '150ms' }}
        />
      </div>

      {/* Printers — the central thing the user is here to monitor */}
      <div className="animate-in" style={{ animationDelay: '200ms' }}>
        <PrintersPanel />
      </div>

      {/* Bottom row — paired peripheral status: Scale on the left, microSD
          on the right. Stacks on mobile; two equal columns from sm: up.
          These are background-info (less actionable than printers) so they
          land at the bottom of the page. */}
      <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
        {/* Scale card */}
        <div
          className="animate-in bg-surface-card border border-surface-border rounded-card p-4 shadow-[0_0_30px_rgba(240,180,41,0.03)]"
          style={{ animationDelay: '250ms' }}
        >
          <div className="text-[10px] uppercase tracking-widest text-text-muted font-medium mb-2">Scale</div>
          <div className="flex items-center gap-3 flex-wrap">
            <StatusDot status={scale?.connected ? 'connected' : 'disconnected'} />
            {!scale?.connected ? (
              <span className="text-lg text-text-muted">Not discovered</span>
            ) : hasReading && typeof grams === 'number' ? (
              <>
                <span className="font-data tabular-nums text-3xl text-text-data">
                  {grams.toFixed(precision)}
                  <span className="text-xl text-text-muted ml-1">g</span>
                </span>
                <span className={`px-2 py-0.5 rounded border text-[11px] uppercase tracking-wide font-medium ${stateClass(wState)}`}>
                  {wState}
                </span>
              </>
            ) : (
              <span className="font-data tabular-nums text-3xl text-text-muted">
                ‒‒
                <span className="text-sm normal-case ml-2 font-sans tracking-normal">waiting…</span>
              </span>
            )}
          </div>
          {scale?.connected && (
            <div className="mt-2 flex items-center flex-wrap gap-x-3 gap-y-1 text-xs font-mono text-text-muted">
              <span>{scale.name ?? 'scale'} @ {scale.ip}</span>
              {scale.telemetry?.fw_version && (
                <span>· v{scale.telemetry.fw_version}</span>
              )}
              {scaleUptime != null && scaleUptime > 0 && (
                <span>· up {formatUptime(scaleUptime)}</span>
              )}
              {scale.telemetry && scale.telemetry.free_heap > 0 && (
                <span>· {(scale.telemetry.free_heap / 1024).toFixed(0)} K free</span>
              )}
            </div>
          )}
        </div>

        {/* microSD card */}
        <div
          className="animate-in bg-surface-card border border-surface-border rounded-card p-4"
          style={{ animationDelay: '300ms' }}
        >
          <div className="flex items-center justify-between mb-2">
            <div className="text-[10px] uppercase tracking-widest text-text-muted font-medium">microSD</div>
            <div className="flex items-center gap-2">
              <StatusDot status={fw?.sd_mounted ? 'connected' : 'disconnected'} />
              <span className="text-xs text-text-secondary">
                {fw?.sd_mounted ? 'Mounted' : 'Not inserted'}
              </span>
            </div>
          </div>
          {fw?.sd_mounted ? (
            <>
              {/* Used / total — explicit unit on both sides so "18" doesn't
                  read as "18 bytes" or "18 GB". Also surface the percentage
                  next to the bar since the raw MB number on its own doesn't
                  convey "how full is this card". */}
              <div className="flex items-baseline gap-2 font-data tabular-nums">
                <span className="text-3xl text-text-data">{fmtMB(fw.sd_used)}</span>
                <span className="text-base text-text-muted">MB</span>
                <span className="text-base text-text-muted">/ {fmtGB(fw.sd_total)} GB</span>
                <span className="text-base text-text-muted ml-auto">
                  {((fw.sd_used / fw.sd_total) * 100).toFixed(1)}%
                </span>
              </div>
              <div className="mt-2 h-1.5 w-full rounded bg-surface-input overflow-hidden">
                <div
                  className="h-full bg-brand-500 transition-all duration-300"
                  style={{ width: `${Math.min(100, (fw.sd_used / fw.sd_total) * 100)}%` }}
                />
              </div>
            </>
          ) : (
            <div className="text-sm text-text-muted">Insert a microSD card to enable local spool backups.</div>
          )}
        </div>
      </div>
    </div>
  );
}
