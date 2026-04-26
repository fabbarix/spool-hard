import { useState } from 'react';
import { Printer, Plus, Trash2, RefreshCw } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { PasswordField } from '@spoolhard/ui/components/PasswordField';
import { Button } from '@spoolhard/ui/components/Button';
import { StatusDot } from '@spoolhard/ui/components/StatusDot';
import { TextScanner } from '@spoolhard/ui/components/TextScanner';
import { usePrinters, useUpsertPrinter, useDeletePrinter, useRefreshPrinters, type Printer as P } from '../../hooks/usePrinters';
import { useDiscoveredPrinters, type DiscoveredPrinter } from '../../hooks/useDiscoveredPrinters';

const MAX_PRINTERS = 5;

function linkToDot(l: P['state']['link']): 'connected' | 'connecting' | 'disconnected' {
  if (l === 'connected') return 'connected';
  if (l === 'connecting') return 'connecting';
  return 'disconnected';
}

export function PrintersSection() {
  const { data: printers } = usePrinters();
  const upsert = useUpsertPrinter();
  const del    = useDeletePrinter();
  const refresh = useRefreshPrinters();

  const [form, setForm] = useState({ name: '', serial: '', ip: '', access_code: '' });
  const [adding, setAdding] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const atLimit = (printers?.length ?? 0) >= MAX_PRINTERS;

  const submit = async () => {
    setError(null);
    if (!form.serial || !form.access_code) {
      setError('Serial and access code are required');
      return;
    }
    try {
      await upsert.mutateAsync(form);
      setForm({ name: '', serial: '', ip: '', access_code: '' });
      setAdding(false);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  return (
    <SectionCard
      title="Bambu Lab Printers"
      icon={<Printer size={16} />}
      description="Up to 5 printers paired over local MQTT on port 8883. Access code is on the printer under Settings → WLAN."
    >
      <div className="space-y-2">
        {(printers ?? []).map((p) => <PrinterRow key={p.serial} p={p} onDelete={() => del.mutate(p.serial)} />)}
        {(printers ?? []).length === 0 && !adding && (
          <div className="text-sm text-text-muted py-2">No printers configured.</div>
        )}
      </div>

      {adding ? (
        <div className="mt-4 p-3 rounded-md border border-surface-border bg-surface-input space-y-3">
          <DiscoveredList
            onPick={(d) => setForm((f) => ({ ...f, serial: d.serial, ip: d.ip, name: f.name || d.model || d.serial }))}
          />
          <InputField
            label="Name"
            value={form.name}
            onChange={(e) => setForm({ ...form, name: e.target.value })}
            placeholder="living-room X1C"
          />
          <ScannableField
            label="Serial"
            value={form.serial}
            onChange={(v) => setForm({ ...form, serial: v })}
            placeholder="03919D1234567890"
            // Bambu serials are alphanumeric, 15+ chars.
            accept={(t) => /^[A-Z0-9]{12,}$/i.test(t)}
            normalize={(t) => t.replace(/\s+/g, '').toUpperCase()}
          />
          <ScannableField
            label="IP address"
            value={form.ip}
            onChange={(v) => setForm({ ...form, ip: v })}
            placeholder="192.168.1.42"
            accept={(t) => /^\d{1,3}(\.\d{1,3}){3}$/.test(t)}
          />
          <ScannableField
            label="Access code"
            value={form.access_code}
            onChange={(v) => setForm({ ...form, access_code: v })}
            placeholder="8-char code"
            // Bambu access codes are 8 digits. If the scan picks them up as
            // text we still accept shorter blocks — user can pick manually.
            accept={(t) => /^\d{6,10}$/.test(t)}
            secret
          />
          {error && <div className="text-xs text-status-error">{error}</div>}
          <div className="flex gap-2">
            <Button onClick={submit} disabled={upsert.isPending}>
              {upsert.isPending ? 'Saving…' : 'Save'}
            </Button>
            <Button variant="secondary" onClick={() => { setAdding(false); setError(null); }}>Cancel</Button>
          </div>
        </div>
      ) : (
        <div className="mt-3 flex items-center gap-2">
          <Button onClick={() => setAdding(true)} disabled={atLimit}>
            <span className="inline-flex items-center gap-1.5">
              <Plus size={16} />
              Add printer{atLimit ? ` (max ${MAX_PRINTERS})` : ''}
            </span>
          </Button>
          <Button
            variant="secondary"
            onClick={() => refresh.mutate()}
            disabled={refresh.isPending}
            title="Re-probe the LAN over SSDP and force-reconnect every configured printer."
          >
            <span className="inline-flex items-center gap-1.5">
              <RefreshCw size={16} className={refresh.isPending ? 'animate-spin' : ''} />
              {refresh.isPending ? 'Refreshing…' : 'Refresh'}
            </span>
          </Button>
        </div>
      )}
    </SectionCard>
  );
}

function DiscoveredList({ onPick }: { onPick: (d: DiscoveredPrinter) => void }) {
  const { data, isLoading } = useDiscoveredPrinters();
  if (isLoading) return null;
  if (!data || data.length === 0) {
    return (
      <div className="text-xs text-text-muted">
        No printers discovered on the LAN yet — they broadcast every ~30 s. You can still enter one manually below.
      </div>
    );
  }
  return (
    <div>
      <div className="text-xs text-text-muted mb-2">Discovered on network — tap to fill</div>
      <div className="space-y-1.5">
        {data.map((d) => (
          <button
            key={d.serial}
            type="button"
            onClick={() => onPick(d)}
            disabled={d.configured}
            className={`w-full text-left p-2 rounded border text-xs font-mono transition-colors ${
              d.configured
                ? 'border-surface-border bg-surface-card/40 text-text-muted cursor-not-allowed'
                : 'border-surface-border bg-surface-card hover:border-brand-500 hover:text-brand-400 cursor-pointer'
            }`}
          >
            <div className="flex items-center gap-2">
              <span className="text-text-primary truncate flex-1">{d.serial}</span>
              <span className="text-text-muted">{d.ip}</span>
              {d.configured && <span className="text-[10px] uppercase tracking-wider text-text-muted">configured</span>}
            </div>
            {d.model && <div className="text-text-muted truncate">{d.model}</div>}
          </button>
        ))}
      </div>
    </div>
  );
}

interface ScannableFieldProps {
  label: string;
  value: string;
  onChange: (v: string) => void;
  placeholder?: string;
  accept?: (t: string) => boolean;
  normalize?: (t: string) => string;
  secret?: boolean;
}

function ScannableField({ label, value, onChange, placeholder, accept, normalize, secret }: ScannableFieldProps) {
  return (
    <div className="flex items-end gap-2">
      <div className="flex-1">
        {secret ? (
          <PasswordField
            label={label}
            value={value}
            onChange={(e) => onChange(e.target.value)}
            placeholder={placeholder}
          />
        ) : (
          <InputField
            label={label}
            value={value}
            onChange={(e) => onChange(e.target.value)}
            placeholder={placeholder}
          />
        )}
      </div>
      <TextScanner
        onText={(t) => onChange(normalize ? normalize(t) : t)}
        accept={accept}
        normalize={normalize}
      />
    </div>
  );
}

function PrinterRow({ p, onDelete }: { p: P; onDelete: () => void }) {
  const s = p.state;
  const link = linkToDot(s.link);
  const detail =
    s.link === 'connected'
      ? s.gcode_state
        ? `${s.gcode_state}${typeof s.progress_pct === 'number' && s.progress_pct >= 0 ? ` · ${s.progress_pct}%` : ''}`
        : 'online'
      : s.error ?? s.link;

  return (
    <div className="flex items-center gap-3 p-3 bg-surface-input rounded-md border border-surface-border">
      <StatusDot status={link} />
      <div className="flex-1 min-w-0">
        <div className="text-sm font-medium text-text-primary truncate">
          {p.name || p.serial}
        </div>
        <div className="text-xs text-text-muted font-mono truncate">
          {p.ip || 'no IP'} · {p.serial} · {detail}
        </div>
      </div>
      <button onClick={onDelete} className="text-text-muted hover:text-red-400 transition-colors cursor-pointer" aria-label="Remove printer">
        <Trash2 size={16} />
      </button>
    </div>
  );
}
