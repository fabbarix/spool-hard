import { useState, useEffect } from 'react';
import { Globe, RefreshCw, ArrowUpCircle, Cpu, Scale } from 'lucide-react';
import {
  useOtaConfig, useSetOtaConfig, useRunOta, useOtaStatus,
  useOtaCheckNow, useRunOtaScale,
  type OtaStatusScaleT, type OtaPendingProductT, type OtaConfigT,
} from '../../hooks/useOtaConfig';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';

const INTERVAL_CHOICES: { value: number; label: string }[] = [
  { value: 1,   label: 'every hour' },
  { value: 6,   label: 'every 6 hours' },
  { value: 24,  label: 'daily' },
  { value: 168, label: 'weekly' },
];

function fmtTs(ts: number): string {
  if (!ts) return 'never';
  const d = new Date(ts * 1000);
  return d.toLocaleString();
}

function statusLabel(s: string): { text: string; tone: 'ok' | 'warn' | 'err' | 'idle' } {
  switch (s) {
    case 'ok':          return { text: 'last check OK', tone: 'ok' };
    case 'network':     return { text: 'network error',  tone: 'err' };
    case 'http_error':  return { text: 'HTTP error',     tone: 'err' };
    case 'parse_error': return { text: 'manifest parse error', tone: 'err' };
    case '':            return { text: 'no checks yet',  tone: 'idle' };
    default:            return { text: s,                tone: 'warn' };
  }
}

// Per-product row: name, fw current → latest, fe current → latest, optional
// "Update now" button on the right. Used for both console and scale so the
// banner reads consistently.
function ProductRow({
  icon, label, info, pending, onUpdate, busy,
}: {
  icon: React.ReactNode;
  label: string;
  info: { fwCurrent?: string; fwLatest?: string; feCurrent?: string; feLatest?: string } | null;
  pending: boolean;
  onUpdate?: () => void;
  busy?: boolean;
}) {
  return (
    <div className="flex items-center justify-between gap-3 flex-wrap">
      <div className="flex items-center gap-2 text-sm">
        <span className="text-text-secondary">{icon}</span>
        <span className="text-text font-medium">{label}</span>
        {info ? (
          <>
            <span className="text-text-secondary">fw</span>
            <span className="font-mono text-text">{info.fwCurrent ?? '…'}</span>
            {info.fwLatest && info.fwLatest !== info.fwCurrent && (
              <>→ <span className="font-mono text-brand-500">{info.fwLatest}</span></>
            )}
            <span className="text-text-secondary ml-2">fe</span>
            <span className="font-mono text-text">{info.feCurrent ?? '…'}</span>
            {info.feLatest && info.feLatest !== info.feCurrent && (
              <>→ <span className="font-mono text-brand-500">{info.feLatest}</span></>
            )}
          </>
        ) : (
          <span className="text-text-secondary italic">unavailable</span>
        )}
      </div>
      {pending && onUpdate && (
        <Button onClick={onUpdate} disabled={busy}>
          <ArrowUpCircle size={14} className="inline mr-1" />
          {busy ? 'Starting…' : 'Update now'}
        </Button>
      )}
    </div>
  );
}

function ScaleLinkPill({ link }: { link: OtaStatusScaleT['link'] }) {
  const map = {
    online:  { text: 'scale online',  cls: 'bg-teal-500/15 text-teal-300 border-teal-500/30' },
    waiting: { text: 'scale waiting', cls: 'bg-amber-500/15 text-amber-300 border-amber-500/30' },
    offline: { text: 'scale offline', cls: 'bg-red-500/15 text-red-300 border-red-500/30' },
  } as const;
  const m = map[link] ?? map.offline;
  return <span className={`text-xs px-2 py-0.5 rounded-full border ${m.cls}`}>{m.text}</span>;
}

export function OtaSection() {
  const cfg     = useOtaConfig();
  const status  = useOtaStatus();
  const save    = useSetOtaConfig();
  const run     = useRunOta();
  const runScale = useRunOtaScale();
  const check   = useOtaCheckNow();

  const [url, setUrl]                 = useState('');
  const [useSsl, setUseSsl]           = useState(false);
  const [verifySsl, setVerifySsl]     = useState(false);
  const [checkEnabled, setCheckEnabled] = useState(true);
  const [intervalH, setIntervalH]     = useState(24);

  useEffect(() => {
    if (cfg.data) {
      setUrl(cfg.data.url);
      setUseSsl(cfg.data.use_ssl);
      setVerifySsl(cfg.data.verify_ssl);
      setCheckEnabled(cfg.data.check_enabled);
      setIntervalH(cfg.data.check_interval_h);
    }
  }, [cfg.data]);

  // Auto-save helper. Toggles + select call this synchronously with their
  // new value (don't trust setState being applied yet); the URL input fires
  // it from onBlur with no override (uses the latest committed local
  // state). Toggling Use SSL off also forces verify_ssl off so the
  // persisted state stays internally consistent — same rule the firmware
  // enforces server-side.
  const commit = (override: Partial<OtaConfigT> = {}) => {
    const body: OtaConfigT = {
      url, use_ssl: useSsl, verify_ssl: verifySsl,
      check_enabled: checkEnabled, check_interval_h: intervalH,
      ...override,
    };
    if (!body.use_ssl) body.verify_ssl = false;
    save.mutate(body);
  };

  const con: OtaPendingProductT | undefined = status.data?.console;
  const scl: OtaStatusScaleT    | undefined = status.data?.scale;

  const lbl = statusLabel(status.data?.last_check_status ?? '');

  const consolePending = !!con?.pending;
  const scaleOnline    = scl?.link === 'online';
  const scalePending   = !!scl?.pending;
  const anyPending     = consolePending || scalePending;

  return (
    <SectionCard
      title="OTA Updates"
      icon={<Globe size={16} />}
      description="Pulls a per-product manifest.json (firmware + frontend versions, sizes, sha256s) and applies whichever component is newer than the running one. Defaults to the latest GitHub release of fabbarix/spool-hard."
    >
      {/* ── Status / pending updates ── */}
      <div className="rounded-md border border-border bg-input p-4 space-y-3">
        <ProductRow
          icon={<Cpu size={14} />}
          label="Console"
          info={con ? {
            fwCurrent: con.firmware_current,
            fwLatest:  con.firmware_latest,
            feCurrent: con.frontend_current,
            feLatest:  con.frontend_latest,
          } : null}
          pending={consolePending}
          onUpdate={() => run.mutate()}
          busy={run.isPending}
        />
        <div className="flex items-center justify-between">
          <ProductRow
            icon={<Scale size={14} />}
            label="Scale"
            info={scaleOnline ? {
              fwCurrent: scl?.firmware_current,
              fwLatest:  scl?.firmware_latest,
              feCurrent: scl?.frontend_current,
              feLatest:  scl?.frontend_latest,
            } : null}
            pending={scalePending}
            onUpdate={() => runScale.mutate()}
            busy={runScale.isPending}
          />
          <ScaleLinkPill link={scl?.link ?? 'offline'} />
        </div>

        <div className="flex items-center justify-between text-xs flex-wrap gap-2 border-t border-border pt-2">
          <div className="text-text-secondary">
            Last checked: <span className="text-text">{fmtTs(status.data?.last_check_ts ?? 0)}</span>
            {' · '}
            <span className={
              lbl.tone === 'ok'   ? 'text-teal-400' :
              lbl.tone === 'warn' ? 'text-amber-400' :
              lbl.tone === 'err'  ? 'text-red-400' :
                                    'text-text-secondary'
            }>{lbl.text}</span>
            {status.data?.check_in_flight && <span className="text-text-secondary"> · checking now…</span>}
          </div>
          <Button
            variant="secondary"
            onClick={() => check.mutate()}
            disabled={check.isPending || status.data?.check_in_flight}
          >
            <RefreshCw size={14} className="inline mr-1" />
            Check now
          </Button>
        </div>

        {anyPending && (
          <div className="rounded-md border border-brand-500/30 bg-brand-500/10 p-3 text-sm text-text">
            Update available for{' '}
            {consolePending && scalePending ? 'console + scale'
              : consolePending ? 'console'
              : 'scale'}
            . Use the per-product button above to apply.
          </div>
        )}
        {runScale.isError && (
          <div className="rounded-md border border-red-500/30 bg-red-500/10 p-3 text-sm text-red-300">
            {(runScale.error as Error)?.message || 'Failed to trigger scale update'}
          </div>
        )}
      </div>

      {/* ── Settings ── */}
      <InputField
        label="Manifest URL"
        value={url}
        onChange={(e) => setUrl(e.target.value)}
        onBlur={() => { if (cfg.data && url !== cfg.data.url) commit({ url }); }}
      />
      {/* All toggles + interval on one row to save vertical space. Wraps on
          narrow viewports. Auto-saves on every interaction — no Save button. */}
      <div className="flex items-center gap-4 flex-wrap text-sm text-text-secondary">
        <label className="flex items-center gap-2 cursor-pointer">
          <input
            type="checkbox"
            checked={useSsl}
            onChange={(e) => { const v = e.target.checked; setUseSsl(v); commit({ use_ssl: v }); }}
            className="rounded border-surface-border accent-brand-500"
          />
          Use SSL
        </label>
        <label className="flex items-center gap-2 cursor-pointer">
          <input
            type="checkbox"
            checked={verifySsl}
            disabled={!useSsl}
            onChange={(e) => { const v = e.target.checked; setVerifySsl(v); commit({ verify_ssl: v }); }}
            className="rounded border-surface-border accent-brand-500 disabled:opacity-50"
          />
          Verify SSL certificate
        </label>
        <label className="flex items-center gap-2 cursor-pointer">
          <input
            type="checkbox"
            checked={checkEnabled}
            onChange={(e) => { const v = e.target.checked; setCheckEnabled(v); commit({ check_enabled: v }); }}
            className="rounded border-surface-border accent-brand-500"
          />
          Periodically check for updates
        </label>
        {checkEnabled && (
          <div className="flex items-center gap-2">
            <span>Check</span>
            <select
              value={intervalH}
              onChange={(e) => { const v = Number(e.target.value); setIntervalH(v); commit({ check_interval_h: v }); }}
              className="rounded-md border border-border bg-input px-2 py-1 text-text focus:border-brand-500 outline-none"
            >
              {INTERVAL_CHOICES.map((c) => (
                <option key={c.value} value={c.value}>{c.label}</option>
              ))}
            </select>
          </div>
        )}
        {save.isPending && <span className="text-xs italic">saving…</span>}
      </div>
    </SectionCard>
  );
}
