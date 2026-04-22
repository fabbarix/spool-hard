import { useState, useEffect } from 'react';
import { Globe, RefreshCw, ArrowUpCircle } from 'lucide-react';
import {
  useOtaConfig, useSetOtaConfig, useOtaStatus, useOtaCheckNow, useRunOta,
} from '../../hooks/useOtaConfig';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';
import type { OtaConfig } from '../../types/api';

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

export function OtaSection() {
  const cfg     = useOtaConfig();
  const status  = useOtaStatus();
  const save    = useSetOtaConfig();
  const run     = useRunOta();
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
  const commit = (override: Partial<OtaConfig> = {}) => {
    const body: OtaConfig = {
      url, use_ssl: useSsl, verify_ssl: verifySsl,
      check_enabled: checkEnabled, check_interval_h: intervalH,
      ...override,
    };
    if (!body.use_ssl) body.verify_ssl = false;
    save.mutate(body);
  };

  const sc           = status.data?.scale;
  const fwCurrent    = sc?.firmware_current;
  const fwLatest     = sc?.firmware_latest;
  const feCurrent    = sc?.frontend_current;
  const feLatest     = sc?.frontend_latest;
  const pending      = !!sc?.pending;

  const lbl = statusLabel(status.data?.last_check_status ?? '');

  return (
    <SectionCard
      title="OTA Updates"
      icon={<Globe size={16} />}
      description="Pulls a per-product manifest.json (firmware + frontend versions, sizes, sha256s) and applies whichever component is newer than the running one. Defaults to the latest GitHub release of fabbarix/spool-hard."
    >
      {/* ── Status / pending updates ── */}
      <div className="rounded-md border border-border bg-input p-4 space-y-2">
        <div className="flex items-baseline justify-between flex-wrap gap-2">
          <div className="text-sm text-text-secondary">
            Scale firmware:{' '}
            <span className="font-mono text-text">{fwCurrent ?? '…'}</span>
            {fwLatest && fwLatest !== fwCurrent && (
              <> → <span className="font-mono text-brand-500">{fwLatest}</span></>
            )}
          </div>
          <div className="text-sm text-text-secondary">
            Frontend:{' '}
            <span className="font-mono text-text">{feCurrent ?? '…'}</span>
            {feLatest && feLatest !== feCurrent && (
              <> → <span className="font-mono text-brand-500">{feLatest}</span></>
            )}
          </div>
        </div>
        <div className="flex items-center justify-between text-xs flex-wrap gap-2">
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
        {pending && (
          <div className="rounded-md border border-brand-500/30 bg-brand-500/10 p-3 text-sm text-text mt-2 flex items-center justify-between gap-2 flex-wrap">
            <span>
              Update available
              {fwLatest && fwLatest !== fwCurrent && <> · firmware <span className="font-mono text-brand-500">{fwLatest}</span></>}
              {feLatest && feLatest !== feCurrent && <> · frontend <span className="font-mono text-brand-500">{feLatest}</span></>}
            </span>
            <Button onClick={() => run.mutate()} disabled={run.isPending}>
              <ArrowUpCircle size={14} className="inline mr-1" />
              {run.isPending ? 'Starting…' : 'Update now'}
            </Button>
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
