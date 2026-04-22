import { StatusDot } from '@spoolhard/ui/components/StatusDot';
import { StatCard } from '@spoolhard/ui/components/StatCard';
import { useScaleLink } from '../hooks/useScaleLink';
import { useFirmwareInfo } from '../hooks/useFirmwareInfo';
import { useSpools } from '../hooks/useSpools';
import { PrintersPanel } from '../components/dashboard/PrintersPanel';

function fmtMB(bytes: number) { return (bytes / 1024 / 1024).toFixed(0); }
function fmtGB(bytes: number) { return (bytes / 1024 / 1024 / 1024).toFixed(1); }

export function DashboardPage() {
  const { data: scale }  = useScaleLink();
  const { data: fw }     = useFirmwareInfo();
  const { data: spools } = useSpools(0, 1);

  return (
    <div className="space-y-4">
      {/* Scale (full width) */}
      <StatCard
        label="Scale"
        value={
          <span className="flex items-center gap-3">
            <StatusDot status={scale?.connected ? 'connected' : 'disconnected'} />
            <span className="text-lg text-text-primary">
              {scale?.connected
                ? `${scale.name ?? 'scale'} @ ${scale.ip}`
                : 'Not discovered'}
            </span>
          </span>
        }
        dataColor={false}
        className="shadow-[0_0_30px_rgba(240,180,41,0.03)]"
        style={{ animationDelay: '0ms' }}
      />

      {/* Metrics row */}
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
        <StatCard
          label="Spools"
          value={spools?.total ?? '--'}
          style={{ animationDelay: '50ms' }}
        />
        <StatCard
          label="Firmware"
          value={fw?.fw_version ?? '--'}
          dataColor={false}
          style={{ animationDelay: '100ms' }}
        />
        <StatCard
          label="Heap"
          value={fw ? `${(fw.free_heap / 1024).toFixed(0)} K` : '--'}
          style={{ animationDelay: '150ms' }}
        />
        <StatCard
          label="PSRAM"
          value={fw ? `${(fw.psram_free / 1024).toFixed(0)} K` : '--'}
          style={{ animationDelay: '200ms' }}
        />
      </div>

      {/* Printers */}
      <div className="animate-in" style={{ animationDelay: '250ms' }}>
        <PrintersPanel />
      </div>

      {/* microSD card */}
      <div className="animate-in" style={{ animationDelay: '300ms' }}>
        <div className="bg-surface-card border border-surface-border rounded-card p-4">
          <div className="flex items-center justify-between mb-3">
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
              <div className="flex items-baseline gap-2 font-data tabular-nums">
                <span className="text-2xl text-text-data">{fmtMB(fw.sd_used)}</span>
                <span className="text-sm text-text-muted">/ {fmtGB(fw.sd_total)} GB</span>
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
