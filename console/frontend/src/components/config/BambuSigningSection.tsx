import { useEffect, useRef, useState, type DragEvent } from 'react';
import {
  FileKey2, Trash2, ShieldCheck, ShieldOff, ShieldAlert, Upload, ClipboardPaste,
  X, Check, Loader2, Send, FileBadge, KeyRound, ListX, CheckCircle2, XCircle, Clock,
} from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { ConfirmDialog } from '@spoolhard/ui/components/ConfirmDialog';
import {
  useBambuSigning, useSaveBambuSigning, useRemoveBambuSigning,
  useSignTest, useStartSignTest, type BambuSigningStatus,
} from '../../hooks/useBambuSigning';
import { usePrinters } from '../../hooks/usePrinters';
import { parseCertPem, parseCrlPem, daysFromNow } from './pemInfo';

type Slot = 'cert' | 'key' | 'crl';

const SLOTS: { id: Slot; label: string; file: string; required: boolean; icon: typeof FileKey2 }[] = [
  { id: 'cert', label: 'Certificate', file: 'slicer_cert.pem', required: true,  icon: FileBadge },
  { id: 'key',  label: 'Private key', file: 'slicer_key.pem',  required: true,  icon: KeyRound },
  { id: 'crl',  label: 'CRL',         file: 'slicer_crl.pem',  required: true,  icon: ListX },
];

interface Staged { text: string; source: string }   // source: file name or "Pasted"

// Every PEM block in `text`, classified. Lets one paste or drop carrying
// all three blocks fill all three slots.
const PEM_RE = /-----BEGIN ([A-Z0-9 ]+)-----[\s\S]+?-----END \1-----/g;
function classifyPem(text: string): { blocks: Partial<Record<Slot, string>>; encrypted: boolean } {
  const blocks: Partial<Record<Slot, string>> = {};
  let encrypted = false;
  for (const m of text.matchAll(PEM_RE)) {
    const kind = m[1];
    const slot: Slot | null =
      kind === 'CERTIFICATE' ? 'cert'
      : kind === 'X509 CRL' ? 'crl'
      : /PRIVATE KEY$/.test(kind) ? 'key'
      : null;
    if (kind === 'ENCRYPTED PRIVATE KEY' || /Proc-Type: 4,ENCRYPTED/.test(m[0])) encrypted = true;
    // Certificates accumulate: the printer needs the whole chain (leaf
    // first, then the CAs), exactly as slicer_cert.pem holds it.
    if (slot === 'cert') blocks.cert = (blocks.cert ?? '') + m[0] + '\n';
    else if (slot && !blocks[slot]) blocks[slot] = m[0] + '\n';
  }
  return { blocks, encrypted };
}

function daysUntil(ymd?: string): number | null {
  if (!ymd) return null;
  const t = Date.parse(`${ymd}T00:00:00Z`);
  return Number.isNaN(t) ? null : Math.floor((t - Date.now()) / 86_400_000);
}

const kb = (s: string) => `${(s.length / 1024).toFixed(1)} KB`;

export function BambuSigningSection() {
  const { data } = useBambuSigning();
  const save   = useSaveBambuSigning();
  const remove = useRemoveBambuSigning();
  const [confirmRemove, setConfirmRemove] = useState(false);
  const [showInstall, setShowInstall] = useState(false);

  const configured = !!data?.configured;

  return (
    <SectionCard
      title="Command signing"
      icon={<FileKey2 size={16} />}
      description="Lets SpoolHard set filament and K values on printers that are NOT in Developer Mode, by signing each command with the certificate from your own Bambu Studio install. Stored on the console only; never included in backups."
    >
      {configured && data ? (
        <>
          <IdentityPanel
            data={data}
            busy={save.isPending || remove.isPending}
            onToggle={() => save.mutate({ enabled: !data.enabled })}
            onRemove={() => setConfirmRemove(true)}
          />
          {data.enabled && <PrinterTest />}
        </>
      ) : (
        <div className="flex items-start gap-3 rounded-card border border-surface-border bg-surface-input px-4 py-3">
          <ShieldOff size={18} className="mt-0.5 shrink-0 text-text-muted" />
          <div className="text-xs text-text-muted leading-relaxed">
            <div className="text-sm font-medium text-text-primary">No certificate installed</div>
            Commands are sent unsigned, which the printer only accepts in LAN Only + Developer Mode.
            {data?.error && <div className="mt-1 text-status-error">Stored certificate unusable: {data.error}</div>}
          </div>
        </div>
      )}

      {configured && !showInstall ? (
        <button
          type="button"
          onClick={() => setShowInstall(true)}
          className="text-xs text-text-muted underline-offset-2 hover:text-text-primary hover:underline"
        >
          Replace certificate…
        </button>
      ) : (
        <InstallForm
          replacing={configured}
          pending={save.isPending}
          error={save.isError ? save.error.message : undefined}
          onCancel={configured ? () => { setShowInstall(false); save.reset(); } : undefined}
          onInstall={(cert, key, crl, done) =>
            save.mutate(
              { cert_pem: cert, key_pem: key, crl_pem: crl },
              { onSuccess: () => { done(); setShowInstall(false); } },
            )}
        />
      )}
      {remove.isError && <div className="text-xs text-status-error">{remove.error.message}</div>}

      <ConfirmDialog
        open={confirmRemove}
        title="Remove signing certificate?"
        message="The certificate and private key are deleted from the console. Commands go out unsigned again."
        confirmLabel="Remove"
        variant="danger"
        busy={remove.isPending}
        onConfirm={() => remove.mutate(undefined, { onSettled: () => setConfirmRemove(false) })}
        onCancel={() => setConfirmRemove(false)}
      />
    </SectionCard>
  );
}

// ---------------------------------------------------------------- identity

function IdentityPanel({ data, busy, onToggle, onRemove }: {
  data: BambuSigningStatus; busy: boolean; onToggle: () => void; onRemove: () => void;
}) {
  const expiryDays = daysUntil(data.not_after);
  const crlDays = daysUntil(data.crl_next_update);
  const expired = expiryDays !== null && expiryDays < 0;
  const bad = data.revoked || expired;

  const pill = bad
    ? { cls: 'bg-red-500/10 border-red-500/30 text-status-error', icon: ShieldAlert,
        text: data.revoked ? 'Revoked' : 'Expired' }
    : data.enabled
      ? { cls: 'bg-teal-400/10 border-teal-400/30 text-status-ok', icon: ShieldCheck, text: 'Signing commands' }
      : { cls: 'bg-surface-card border-surface-border text-text-muted', icon: ShieldOff, text: 'Disabled' };
  const PillIcon = pill.icon;

  return (
    <div className="rounded-card border border-surface-border bg-surface-input">
      <div className="flex flex-wrap items-center justify-between gap-2 border-b border-surface-border px-4 py-3">
        <span className={`inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-xs font-medium ${pill.cls}`}>
          <PillIcon size={14} /> {pill.text}
        </span>
        <div className="flex gap-2">
          <Button variant="secondary" className="!px-3 !py-1.5 text-xs" disabled={busy} onClick={onToggle}>
            {data.enabled ? 'Disable' : 'Enable'}
          </Button>
          <Button variant="danger" className="!px-3 !py-1.5 text-xs" disabled={busy} onClick={onRemove}>
            <span className="inline-flex items-center gap-1"><Trash2 size={13} /> Remove</span>
          </Button>
        </div>
      </div>
      <dl className="grid grid-cols-1 gap-x-6 gap-y-2 px-4 py-3 text-xs sm:grid-cols-[auto_1fr]">
        <dt className="text-text-muted">Certificate ID</dt>
        <dd className="break-all font-mono text-text-primary">{data.cert_id}</dd>
        <dt className="text-text-muted">Subject</dt>
        <dd className="break-all font-mono text-text-primary">{data.subject}</dd>
        <dt className="text-text-muted">Valid until</dt>
        <dd className={expired || (expiryDays !== null && expiryDays < 30) ? 'text-status-error' : 'text-text-primary'}>
          {data.not_after}
          {expiryDays !== null && (
            <span className="ml-2 text-text-muted">{expired ? 'expired' : `${expiryDays} days left`}</span>
          )}
        </dd>
        <dt className="text-text-muted">CRL</dt>
        <dd className="text-text-primary">
          {data.crl_present ? (
            <>
              {data.revoked ? <span className="text-status-error">lists this certificate as revoked</span> : 'not revoked'}
              <span className={`ml-2 ${crlDays !== null && crlDays < 0 ? 'text-brand-400' : 'text-text-muted'}`}>
                next update {data.crl_next_update}{crlDays !== null && crlDays < 0 && ' (stale)'}
              </span>
            </>
          ) : <span className="text-status-error">none installed: printers refuse the certificate without it</span>}
        </dd>
        <dt className="text-text-muted">Chain</dt>
        <dd className={(data.chain_certs ?? 0) < 3 ? 'text-status-error' : 'text-text-primary'}>
          {data.chain_certs ?? 0} certificate{data.chain_certs === 1 ? '' : 's'}
          {(data.chain_certs ?? 0) < 3 && <span className="ml-2">the printer needs the full chain (certificate + 2 CAs); re-install slicer_cert.pem</span>}
        </dd>
      </dl>
    </div>
  );
}

// ------------------------------------------------------------ printer test

function PrinterTest() {
  const { data: printers } = usePrinters();
  const connected = (printers ?? []).filter((p) => p.state.link === 'connected');
  const [serial, setSerial] = useState<string>();
  const chosen = connected.find((p) => p.serial === serial) ?? connected[0];
  const start = useStartSignTest();
  const { data: res } = useSignTest(chosen?.serial);

  const running = start.isPending || ['queued', 'registering', 'sent'].includes(res?.state ?? '');
  // Only show a result for a test started on this page view.
  const [started, setStarted] = useState(false);
  const shown = started ? res : undefined;

  return (
    <div className="space-y-3 rounded-card border border-surface-border bg-surface-input px-4 py-3">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div className="text-xs text-text-muted leading-relaxed">
          <div className="text-sm font-medium text-text-primary">Test with printer</div>
          Sends one signed command that re-applies a loaded slot's current filament, so nothing changes.
        </div>
        <div className="flex items-center gap-2">
          {connected.length > 1 && (
            <select
              value={chosen?.serial}
              onChange={(e) => { setSerial(e.target.value); setStarted(false); }}
              disabled={running}
              className="rounded-button border border-surface-border bg-surface-card px-2 py-1.5 text-xs text-text-primary focus:outline-none focus:ring-1 focus:ring-brand-500"
            >
              {connected.map((p) => <option key={p.serial} value={p.serial}>{p.name || p.serial}</option>)}
            </select>
          )}
          <Button
            className="!px-3 !py-1.5 text-xs"
            disabled={!chosen || running}
            onClick={() => { setStarted(true); start.mutate(chosen!.serial); }}
          >
            <span className="inline-flex items-center gap-1.5">
              {running ? <Loader2 size={13} className="animate-spin" /> : <Send size={13} />}
              {running ? 'Testing…' : connected.length === 1 ? `Test on ${chosen?.name || chosen?.serial}` : 'Test'}
            </span>
          </Button>
        </div>
      </div>
      {!chosen ? (
        <div className="text-xs text-text-muted">No printer is connected right now.</div>
      ) : <Registration state={chosen.state.signing_cert} detail={chosen.state.signing_cert_detail} />}
      {started && start.isError && <ResultLine tone="error" icon={XCircle} title="Could not start the test" detail={start.error.message} />}
      {shown && <TestResult r={shown} />}
    </div>
  );
}

// The printer forgets registered certificates on power cycle; the console
// re-registers on connect and whenever a signed command comes back 84033545.
function Registration({ state, detail }: { state?: string; detail?: string }) {
  const [cls, text] =
    state === 'trusted'    ? ['text-status-ok', 'Certificate registered on this printer']
    : state === 'checking' ? ['text-text-muted', 'Checking the printer’s trusted certificates…']
    : state === 'installing' ? ['text-text-muted', 'Registering the certificate with the printer…']
    : state === 'failed'   ? ['text-status-error', `Registration failed${detail ? `: ${detail}` : ''}`]
    : ['text-text-muted', 'Not registered yet; the test registers it first'];
  const busy = state === 'checking' || state === 'installing';
  return (
    <div className={`flex items-center gap-1.5 text-xs ${cls}`}>
      {busy ? <Loader2 size={12} className="animate-spin" />
        : state === 'trusted' ? <CheckCircle2 size={12} />
        : state === 'failed' ? <XCircle size={12} /> : <Clock size={12} />}
      {text}
    </div>
  );
}

function TestResult({ r }: { r: NonNullable<ReturnType<typeof useSignTest>['data']> }) {
  const where = r.slot ? ` (${r.slot})` : '';
  const secs = `${(r.elapsed_ms / 1000).toFixed(1)} s`;
  switch (r.state) {
    case 'registering':
      return <ResultLine tone="busy" icon={Loader2} spin title="Registering the certificate with the printer…"
        detail="Sends the certificate chain and CRL, then waits for the printer to list it (up to ~40 s)." />;
    case 'queued':
    case 'sent':
      return <ResultLine tone="busy" icon={Loader2} spin title={`Waiting for the printer${where}…`} />;
    case 'accepted':
      return <ResultLine tone="ok" icon={CheckCircle2} title="Accepted: the printer trusts this certificate"
        detail={`Signed command acknowledged in ${secs}${where}. Filament and K updates will work without Developer Mode.`} />;
    case 'refused':
      return <ResultLine tone="error" icon={XCircle}
        title={r.err === 84033545 ? 'Refused: the certificate is not registered on the printer'
             : r.err === 84033547 ? 'Refused: incomplete certificate chain or CRL'
             : r.err === 84033548 ? 'Refused: the signature does not match the certificate'
             : r.err === 84033546 ? 'Refused: the printer could not parse the command'
             : r.err === 84033543 ? 'Refused: the printer saw no signature'
             : 'Refused by the printer'}
        detail={`${r.reason || 'no reason given'}${r.err ? ` (code ${r.err})` : ''}${where}`} />;
    case 'timeout':
      return <ResultLine tone="warn" icon={Clock} title="No reply"
        detail={`The printer did not answer within 10 s${where}. Some firmware silently drops commands it cannot verify.`} />;
    case 'failed':
      return <ResultLine tone="error" icon={XCircle} title="Test not sent" detail={r.reason} />;
    default:
      return null;
  }
}

const TONES = {
  ok:    'border-teal-400/30 bg-teal-400/5 text-status-ok',
  error: 'border-red-500/30 bg-red-500/5 text-status-error',
  warn:  'border-brand-500/30 bg-brand-500/5 text-brand-400',
  busy:  'border-surface-border bg-surface-card text-text-secondary',
};

function ResultLine({ tone, icon: Icon, spin, title, detail }: {
  tone: keyof typeof TONES; icon: typeof Check; spin?: boolean; title: string; detail?: string;
}) {
  return (
    <div className={`flex items-start gap-2.5 rounded-button border px-3 py-2.5 ${TONES[tone]}`} role="status">
      <Icon size={16} className={`mt-0.5 shrink-0 ${spin ? 'animate-spin' : ''}`} />
      <div className="text-xs leading-relaxed">
        <div className="font-medium">{title}</div>
        {detail && <div className="text-text-muted">{detail}</div>}
      </div>
    </div>
  );
}

// ------------------------------------------------------------ install form

function InstallForm({ replacing, pending, error, onInstall, onCancel }: {
  replacing: boolean;
  pending: boolean;
  error?: string;
  onInstall: (cert: string, key: string, crl: string, done: () => void) => void;
  onCancel?: () => void;
}) {
  const [staged, setStaged] = useState<Record<Slot, Staged | null>>({ cert: null, key: null, crl: null });
  const [slotErr, setSlotErr] = useState<Partial<Record<Slot, string>>>({});

  // Route every PEM block in `text` to its slot; `target` gets an error if
  // the input held nothing usable for it.
  const ingest = (target: Slot, text: string, source: string) => {
    const { blocks, encrypted } = classifyPem(text);
    const found = Object.keys(blocks) as Slot[];
    const errs: Partial<Record<Slot, string>> = {};
    if (encrypted) errs.key = 'Password-protected keys are not supported; export the key unencrypted.';
    else if (found.length === 0) errs[target] = 'No PEM block found (expected "-----BEGIN …-----").';
    setSlotErr((cur) => {
      const next = { ...cur };
      for (const s of found) delete next[s];
      delete next[target];
      return { ...next, ...errs };
    });
    if (encrypted) { delete blocks.key; }
    setStaged((cur) => {
      const next = { ...cur };
      for (const s of Object.keys(blocks) as Slot[]) next[s] = { text: blocks[s]!, source };
      return next;
    });
  };

  const clear = (s: Slot) => {
    setStaged((cur) => ({ ...cur, [s]: null }));
    setSlotErr((cur) => { const n = { ...cur }; delete n[s]; return n; });
  };

  const ready = !!staged.cert && !!staged.key && !!staged.crl;

  return (
    <div className="space-y-3 border-t border-surface-border pt-4">
      <div className="flex items-center justify-between">
        <div className="text-sm font-medium text-text-primary">
          {replacing ? 'Replace certificate' : 'Install certificate'}
        </div>
        {onCancel && (
          <button type="button" onClick={onCancel} className="text-xs text-text-muted hover:text-text-primary">
            Cancel
          </button>
        )}
      </div>
      <p className="text-xs text-text-muted leading-relaxed">
        Choose, drop or paste each file. Pasting text that holds several PEM blocks fills every matching slot at once.
      </p>
      <div className="grid grid-cols-1 gap-3 md:grid-cols-3">
        {SLOTS.map((s) => (
          <PemSlot
            key={s.id}
            slot={s}
            staged={staged[s.id]}
            error={slotErr[s.id]}
            disabled={pending}
            onText={(text, source) => ingest(s.id, text, source)}
            onClear={() => clear(s.id)}
          />
        ))}
      </div>
      <div className="flex flex-wrap items-center gap-3">
        <Button
          disabled={!ready || pending}
          onClick={() => onInstall(staged.cert!.text, staged.key!.text, staged.crl?.text ?? '',
            () => setStaged({ cert: null, key: null, crl: null }))}
        >
          <span className="inline-flex items-center gap-1.5">
            {pending ? <Loader2 size={14} className="animate-spin" /> : <ShieldCheck size={14} />}
            {pending ? 'Checking…' : 'Install'}
          </span>
        </Button>
        {!ready && <span className="text-xs text-text-muted">Certificate, private key and CRL are all required.</span>}
      </div>
      {error && <ResultLine tone="error" icon={XCircle} title="Not installed" detail={error} />}
    </div>
  );
}

function PemSlot({ slot, staged, error, disabled, onText, onClear }: {
  slot: (typeof SLOTS)[number];
  staged: Staged | null;
  error?: string;
  disabled: boolean;
  onText: (text: string, source: string) => void;
  onClear: () => void;
}) {
  const fileRef = useRef<HTMLInputElement>(null);
  const areaRef = useRef<HTMLTextAreaElement>(null);
  const [pasting, setPasting] = useState(false);
  const [draft, setDraft] = useState('');
  const [over, setOver] = useState(false);
  const Icon = slot.icon;

  useEffect(() => { if (pasting) areaRef.current?.focus(); }, [pasting]);

  const readFile = async (f: File | undefined) => {
    if (!f) return;
    if (f.size > 16 * 1024) { onText('', f.name); return; }   // reported as "no PEM block"
    onText(await f.text(), f.name);
  };

  // The async clipboard API needs a secure context (https / localhost); the
  // console is plain http on the LAN, so fall back to a paste box.
  const paste = async () => {
    if (window.isSecureContext && navigator.clipboard?.readText) {
      try {
        const t = await navigator.clipboard.readText();
        if (t.trim()) { onText(t, 'Pasted'); return; }
      } catch { /* permission denied: use the box */ }
    }
    setDraft('');
    setPasting(true);
  };

  const commit = (text: string) => {
    setPasting(false);
    setDraft('');
    if (text.trim()) onText(text, 'Pasted');
  };

  const onDrop = (e: DragEvent) => {
    e.preventDefault();
    setOver(false);
    if (disabled) return;
    const f = e.dataTransfer.files[0];
    if (f) void readFile(f);
    else {
      const t = e.dataTransfer.getData('text/plain');
      if (t) onText(t, 'Dropped text');
    }
  };

  const state = error ? 'error' : staged ? 'ok' : 'empty';

  return (
    <div
      onDragOver={(e) => { e.preventDefault(); if (!disabled) setOver(true); }}
      onDragLeave={() => setOver(false)}
      onDrop={onDrop}
      className={`flex min-h-[148px] flex-col rounded-card border p-3 transition-colors duration-200 ${
        over ? 'border-brand-400 bg-brand-500/10'
        : state === 'error' ? 'border-status-error/40 bg-status-error/5'
        : state === 'ok' ? 'border-teal-400/40 bg-teal-400/5'
        : 'border-dashed border-surface-border bg-surface-input'
      }`}
    >
      <div className="flex items-start justify-between gap-2">
        <div className="flex items-center gap-2">
          <span className={`flex h-7 w-7 items-center justify-center rounded-full ${
            state === 'ok' ? 'bg-teal-400/15 text-status-ok'
            : state === 'error' ? 'bg-red-500/15 text-status-error'
            : 'bg-surface-card text-text-muted'}`}>
            {state === 'ok' ? <Check size={15} /> : <Icon size={15} />}
          </span>
          <div className="leading-tight">
            <div className="text-sm font-medium text-text-primary">
              {slot.label}{!slot.required && <span className="ml-1 text-xs font-normal text-text-muted">optional</span>}
            </div>
            <div className="font-mono text-[11px] text-text-muted">{slot.file}</div>
          </div>
        </div>
        {staged && !pasting && (
          <button type="button" onClick={onClear} disabled={disabled} aria-label={`Clear ${slot.label}`}
            className="rounded p-0.5 text-text-muted transition-colors hover:text-status-error">
            <X size={14} />
          </button>
        )}
      </div>

      {pasting ? (
        <div className="mt-2 flex flex-1 flex-col gap-2">
          <textarea
            ref={areaRef}
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            onPaste={(e) => { e.preventDefault(); commit(e.clipboardData.getData('text')); }}
            onKeyDown={(e) => { if (e.key === 'Escape') setPasting(false); }}
            placeholder={'Press Ctrl+V / ⌘V here\n-----BEGIN …-----'}
            spellCheck={false}
            autoComplete="off"
            rows={3}
            className="w-full flex-1 resize-none rounded-md border border-surface-border bg-surface-body px-2 py-1.5 font-mono text-[11px] text-text-primary focus:outline-none focus:ring-1 focus:ring-brand-500"
          />
          <div className="flex justify-end gap-2">
            <button type="button" onClick={() => setPasting(false)} className="text-xs text-text-muted hover:text-text-primary">Cancel</button>
            <button type="button" onClick={() => commit(draft)} disabled={!draft.trim()}
              className="text-xs font-medium text-brand-400 hover:text-brand-500 disabled:opacity-40">Use text</button>
          </div>
        </div>
      ) : (
        <>
          <div className="mt-2 flex-1 text-xs">
            {error ? <span className="text-status-error">{error}</span>
              : staged ? (
                <>
                  <div className="text-text-secondary">
                    <span className="font-mono">{staged.source}</span>
                    <span className="text-text-muted"> · {kb(staged.text)}</span>
                  </div>
                  <PemDetails slot={slot.id} text={staged.text} />
                </>
              )
              : <span className="text-text-muted">Drop a file here, or:</span>}
          </div>
          <div className="mt-2 flex gap-2">
            <button type="button" disabled={disabled} onClick={() => fileRef.current?.click()}
              className="inline-flex flex-1 items-center justify-center gap-1.5 rounded-button border border-surface-border bg-surface-card px-2 py-1.5 text-xs text-text-primary transition-colors hover:bg-surface-card-hover disabled:opacity-50">
              <Upload size={13} /> {staged ? 'Replace' : 'Choose file'}
            </button>
            <button type="button" disabled={disabled} onClick={paste}
              className="inline-flex flex-1 items-center justify-center gap-1.5 rounded-button border border-surface-border bg-surface-card px-2 py-1.5 text-xs text-text-primary transition-colors hover:bg-surface-card-hover disabled:opacity-50">
              <ClipboardPaste size={13} /> Paste
            </button>
          </div>
          <input
            ref={fileRef}
            type="file"
            accept=".pem,.crt,.cer,.key,.crl,application/x-pem-file,text/plain"
            className="hidden"
            onChange={(e) => { void readFile(e.target.files?.[0]); e.target.value = ''; }}
          />
        </>
      )}
    </div>
  );
}

const ymd = (d: Date) => d.toISOString().slice(0, 10);

// What the staged PEM says about itself, parsed in the browser.
function PemDetails({ slot, text }: { slot: Slot; text: string }) {
  if (slot === 'key') {
    const kind = /BEGIN RSA PRIVATE KEY/.test(text) ? 'RSA private key'
               : /BEGIN EC PRIVATE KEY/.test(text) ? 'EC private key (RSA needed)'
               : 'Private key';
    return <div className="mt-1 text-text-muted">{kind}; matched against the certificate on install.</div>;
  }
  if (slot === 'cert') {
    const c = parseCertPem(text);
    if (!c) return <div className="mt-1 text-brand-400">Could not read the certificate dates.</div>;
    const chain = (text.match(/BEGIN CERTIFICATE/g) ?? []).length;
    const left = daysFromNow(c.notAfter);
    const notYet = daysFromNow(c.notBefore) > 0;
    const tone = left < 0 || notYet ? 'text-status-error' : left < 30 ? 'text-brand-400' : 'text-status-ok';
    return (
      <div className="mt-1 space-y-0.5">
        {c.subject && <div className="truncate font-mono text-text-muted" title={c.subject}>{c.subject}</div>}
        {chain < 3 && <div className="text-status-error">Only {chain} certificate{chain === 1 ? '' : 's'}: the printer needs the full chain (use the whole slicer_cert.pem)</div>}
        <div className={tone}>
          {notYet ? `Not valid until ${ymd(c.notBefore)}`
            : left < 0 ? `Expired ${ymd(c.notAfter)}`
            : `Expires ${ymd(c.notAfter)} · ${left} day${left === 1 ? '' : 's'} left`}
        </div>
      </div>
    );
  }
  const r = parseCrlPem(text);
  if (!r) return <div className="mt-1 text-brand-400">Could not read the CRL dates.</div>;
  if (!r.nextUpdate) return <div className="mt-1 text-text-muted">Issued {ymd(r.thisUpdate)} · no next-update date</div>;
  const left = daysFromNow(r.nextUpdate);
  return (
    <div className="mt-1 space-y-0.5">
      <div className="text-text-muted">Issued {ymd(r.thisUpdate)}</div>
      <div className={left < 0 ? 'text-brand-400' : 'text-status-ok'}>
        {left < 0 ? `Stale since ${ymd(r.nextUpdate)}` : `Next update ${ymd(r.nextUpdate)} · ${left} day${left === 1 ? '' : 's'}`}
      </div>
    </div>
  );
}
