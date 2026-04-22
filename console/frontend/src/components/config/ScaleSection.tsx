import { useState } from 'react';
import { Scale, ExternalLink, Activity, KeyRound, ChevronDown, ChevronRight } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { PasswordField } from '@spoolhard/ui/components/PasswordField';
import { StatusDot } from '@spoolhard/ui/components/StatusDot';
import { useScaleLink, useScaleSecret, useSetScaleSecret } from '../../hooks/useScaleLink';
import { useDiscoveredScales, type DiscoveredScale, type ScaleHandshake } from '../../hooks/useDiscoveredScales';

export function ScaleSection() {
  const { data: link }   = useScaleLink();
  const { data: scales } = useDiscoveredScales();

  const pairedScales = (scales ?? []).filter((s) => s.paired);
  const lastEv = link?.last_event;

  return (
    <SectionCard
      title="Scale"
      icon={<Scale size={16} />}
      description="SpoolHard Scales the console is paired with. Each scale has its own shared secret — expand a row to set it, or tap Open to jump into that scale's own UI."
    >
      {/* Paired scales list */}
      <div>
        <div className="text-xs text-text-muted mb-2">
          Paired {pairedScales.length > 0 ? `(${pairedScales.length})` : ''}
        </div>
        {pairedScales.length === 0 ? (
          <div className="text-sm text-text-muted py-2">
            No scale paired. The console auto-pairs with the first SpoolHard
            Scale it hears SSDP from.
          </div>
        ) : (
          <div className="space-y-1.5">
            {pairedScales.map((s) => <PairedScaleRow key={s.name} s={s} />)}
          </div>
        )}
      </div>

      {/* Last event */}
      <div className="mt-5 pt-4 border-t border-surface-border">
        <div className="flex items-center gap-1.5 text-xs text-text-muted mb-2">
          <Activity size={12} />
          Last event
        </div>
        {lastEv && lastEv.ago_ms >= 0 ? (
          <div className="flex items-baseline flex-wrap gap-x-3 gap-y-1 text-sm">
            {lastEv.scale_name && (
              <span className="font-mono text-text-primary">{lastEv.scale_name}</span>
            )}
            <span className="font-data text-brand-400 tabular-nums text-base">{lastEv.detail}</span>
            <span className="text-xs text-text-muted font-mono">
              {lastEv.kind} · {formatAgo(lastEv.ago_ms)}
            </span>
          </div>
        ) : (
          <div className="text-sm text-text-muted">
            {link?.connected ? 'Waiting for activity…' : 'Scale not connected.'}
          </div>
        )}
      </div>
    </SectionCard>
  );
}

function handshakeToDot(h: ScaleHandshake): 'connected' | 'connecting' | 'disconnected' {
  if (h === 'encrypted')    return 'connected';
  if (h === 'disconnected') return 'disconnected';
  return 'connecting';
}

function handshakeLabel(h: ScaleHandshake): string {
  switch (h) {
    case 'encrypted':    return 'encrypted';
    case 'unencrypted':  return 'not encrypted';
    case 'failed':       return 'handshake failed';
    default:             return 'disconnected';
  }
}

function PairedScaleRow({ s }: { s: DiscoveredScale }) {
  const mdnsHost = s.name ? `${s.name.toLowerCase().replace(/\s+/g, '-')}.local` : s.ip;
  const configUrl = `http://${mdnsHost}/configuration?tab=scale`;
  const status = handshakeToDot(s.handshake);
  const [open, setOpen] = useState(false);

  return (
    <div className="rounded border border-surface-border bg-surface-card">
      <button
        type="button"
        onClick={() => setOpen((v) => !v)}
        className="w-full flex items-center gap-2 p-2 text-left cursor-pointer"
        aria-expanded={open}
      >
        {open ? <ChevronDown size={14} className="text-text-muted" />
              : <ChevronRight size={14} className="text-text-muted" />}
        <StatusDot status={status} />
        <div className="flex-1 min-w-0">
          <div className="flex items-baseline gap-2">
            <span className="text-sm text-text-primary truncate">{s.name}</span>
            <span className="text-[10px] uppercase tracking-wider text-text-muted">
              {handshakeLabel(s.handshake)}
            </span>
          </div>
          <div className="text-xs text-text-muted font-mono truncate">{s.ip} · {mdnsHost}</div>
        </div>
        <a
          href={configUrl}
          target="_blank"
          rel="noopener noreferrer"
          onClick={(e) => e.stopPropagation()}
          className="inline-flex items-center gap-1 text-xs text-text-secondary hover:text-brand-400 cursor-pointer px-2 py-1 rounded border border-surface-border hover:border-brand-500 transition-colors"
          title={`Open ${mdnsHost} config`}
        >
          Open <ExternalLink size={12} />
        </a>
      </button>

      {open && <ScaleSecretPanel name={s.name} />}
    </div>
  );
}

function ScaleSecretPanel({ name }: { name: string }) {
  const { data } = useScaleSecret(name);
  const mut = useSetScaleSecret();
  const [secret, setSecret] = useState('');

  const submit = async () => {
    await mut.mutateAsync({ name, secret });
    setSecret('');
  };

  const clear = async () => {
    await mut.mutateAsync({ name, secret: '' });
    setSecret('');
  };

  return (
    <div className="p-3 border-t border-surface-border bg-surface-input/40 space-y-3">
      <div className="flex items-center gap-1.5 text-xs text-text-muted">
        <KeyRound size={12} />
        Shared secret for <span className="font-mono text-text-primary">{name}</span>
      </div>
      <p className="text-xs text-text-muted">
        HMAC key the console uses to authenticate to this scale. Must match
        the secret set on that scale's Security config page.
      </p>
      <div className="text-xs text-text-muted">
        Current: <span className="font-mono text-text-primary">{data?.preview || 'not set'}</span>
      </div>
      <div className="flex items-end gap-2">
        <div className="flex-1">
          <PasswordField
            label="New secret"
            value={secret}
            onChange={(e) => setSecret(e.target.value)}
            placeholder="Leave blank to clear"
          />
        </div>
        <Button onClick={submit} disabled={!secret || mut.isPending}>
          {mut.isPending ? 'Saving…' : 'Save'}
        </Button>
        {data?.configured && (
          <Button variant="danger" onClick={clear} disabled={mut.isPending}>Clear</Button>
        )}
      </div>
    </div>
  );
}

function formatAgo(ms: number): string {
  if (ms < 1000) return `${ms} ms`;
  if (ms < 60_000) return `${(ms / 1000).toFixed(1)} s ago`;
  if (ms < 3_600_000) return `${Math.round(ms / 60_000)} m ago`;
  return `${Math.round(ms / 3_600_000)} h ago`;
}
