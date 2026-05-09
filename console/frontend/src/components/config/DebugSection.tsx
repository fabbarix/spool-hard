import { useCallback, useEffect, useRef, useState } from 'react';
import { Bug, Pause, Play, Trash2, Copy, Zap, CheckCircle2, XCircle, Loader2, FolderOpen, Download, Eraser, HardDrive, Database, AlertTriangle, FileWarning, RefreshCw } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { useWebSocket, type DebugEntry } from '@spoolhard/ui/providers/WebSocketProvider';
import { useDebugConfig, useUpdateDebugConfig } from '../../hooks/useDebugConfig';
import { useCrashes, useDeleteCrash, useDeleteAllCrashes, fetchCrashText, type CrashEntry } from '../../hooks/useCrashLogs';
import { usePrinters, type Printer } from '../../hooks/usePrinters';

// NDJSON events the firmware emits on the FTP debug chunked response.
// Every line is a single JSON object with a `kind` discriminator.
interface FtpTraceLine {
  kind: 'trace';
  serial: string;
  op: 'probe' | 'list' | 'download';
  step: string;
  code: number;
  text: string;
  elapsed_ms: number;
}
interface FtpDoneLine {
  kind: 'done';
  serial: string;
  op: 'probe' | 'list' | 'download';
  ok: boolean;
  message: string;
  payload?: { entries?: string[]; path?: string; bytes?: number; url?: string };
}
type FtpStreamLine = FtpTraceLine | FtpDoneLine;

// Capped so the ring buffer doesn't grow unbounded over a long debugging
// session — AMS reports land every few seconds and each is 1–5 KB, so ~100
// entries is plenty of recent history without running the tab out of memory.
const MAX_ENTRIES = 100;

export function DebugSection() {
  const { data: cfg, isLoading } = useDebugConfig();
  const update                   = useUpdateDebugConfig();
  const { subscribeDebug }       = useWebSocket();

  const [entries, setEntries] = useState<DebugEntry[]>([]);
  const [paused, setPaused]   = useState(false);
  const pausedRef             = useRef(paused);
  pausedRef.current           = paused;

  // Subscribe once — the WebSocketProvider pushes every ams_raw message
  // through here. We gate via pausedRef so toggling Pause doesn't have to
  // tear down the subscription.
  useEffect(() => {
    const off = subscribeDebug((entry) => {
      if (pausedRef.current) return;
      setEntries((prev) => {
        const next = [...prev, entry];
        if (next.length > MAX_ENTRIES) next.splice(0, next.length - MAX_ENTRIES);
        return next;
      });
    });
    return off;
  }, [subscribeDebug]);

  const toggleAmsLog = useCallback(() => {
    update.mutate({ log_ams: !cfg?.log_ams });
  }, [cfg?.log_ams, update]);

  const copyAll = useCallback(() => {
    const text = entries
      .map((e) => `[${e.ts}] ${e.type}\n${JSON.stringify(e.data, null, 2)}`)
      .join('\n\n');
    navigator.clipboard?.writeText(text).catch(() => { /* clipboard blocked */ });
  }, [entries]);

  return (
    <SectionCard
      title="Debug"
      icon={<Bug size={16} />}
      description="Firmware-side debug toggles. Sessions are not persisted — everything defaults off on boot. Leave disabled during normal use; each AMS report is 1–5 KB of WebSocket traffic."
    >
      <CrashLogsSection />

      <FtpAnalysisProbe />

      <SpoolStorageBackend />

      <div className="flex items-center justify-between gap-3 py-2 border-b border-surface-border">
        <div>
          <div className="text-sm text-text-primary">Log AMS messages</div>
          <div className="text-xs text-text-muted mt-0.5">
            Forwards every <span className="font-mono">print.ams</span> MQTT payload to this
            page so you can inspect the raw JSON Bambu is reporting. Useful when the on-
            device display disagrees with the printer's own panel.
          </div>
        </div>
        <Toggle
          on={!!cfg?.log_ams}
          disabled={isLoading || update.isPending}
          onClick={toggleAmsLog}
        />
      </div>

      <div className="flex items-center justify-between pt-3">
        <div className="text-xs text-text-muted">
          {entries.length} {entries.length === 1 ? 'entry' : 'entries'}
          {entries.length >= MAX_ENTRIES && <span className="text-text-muted"> (oldest dropped)</span>}
          {paused && <span className="text-status-warn ml-2">paused</span>}
        </div>
        <div className="flex items-center gap-1.5">
          <Button variant="secondary" onClick={() => setPaused((p) => !p)} disabled={!cfg?.log_ams}>
            {paused ? <><Play size={13} className="mr-1 inline" />Resume</>
                    : <><Pause size={13} className="mr-1 inline" />Pause</>}
          </Button>
          <Button variant="secondary" onClick={() => setEntries([])} disabled={entries.length === 0}>
            <Trash2 size={13} className="mr-1 inline" />Clear
          </Button>
          <Button variant="secondary" onClick={copyAll} disabled={entries.length === 0}>
            <Copy size={13} className="mr-1 inline" />Copy
          </Button>
        </div>
      </div>

      {entries.length === 0 ? (
        <div className="mt-3 rounded-md border border-dashed border-surface-border p-6 text-center text-xs text-text-muted italic">
          {cfg?.log_ams
            ? 'Waiting for the next AMS report from a connected printer…'
            : 'Toggle "Log AMS messages" on to start capturing.'}
        </div>
      ) : (
        <div className="mt-3 max-h-[480px] overflow-y-auto space-y-2 font-mono text-[11px]">
          {[...entries].reverse().map((e, i) => (
            <LogRow key={entries.length - i} entry={e} />
          ))}
        </div>
      )}
    </SectionCard>
  );
}

function LogRow({ entry }: { entry: DebugEntry }) {
  const [open, setOpen] = useState(false);
  const pretty = JSON.stringify(entry.data, null, 2);
  // Collapsed preview: single-line compact JSON, truncated.
  const preview = JSON.stringify(entry.data);
  return (
    <div className="rounded border border-surface-border bg-surface-input">
      <button
        onClick={() => setOpen((v) => !v)}
        className="w-full flex items-center gap-2 px-2 py-1.5 text-left hover:bg-surface-card-hover"
      >
        <span className="text-text-muted tabular-nums">{entry.ts}</span>
        <span className="text-brand-400 flex-shrink-0">{entry.type}</span>
        {!open && (
          <span className="text-text-muted truncate flex-1">{preview}</span>
        )}
        <span className="text-text-muted text-[10px] flex-shrink-0">{open ? '▴' : '▾'}</span>
      </button>
      {open && (
        <pre className="px-3 py-2 text-[11px] text-text-primary whitespace-pre-wrap break-all border-t border-surface-border">
{pretty}
        </pre>
      )}
    </div>
  );
}

// Crash logs preserved to SD by the firmware. The console snapshots its
// in-memory ring log to /spoolease/logs/current.log every ~3 s; on every
// boot, if the reset reason indicates a panic / watchdog / brownout, the
// previous session's log is preserved under /spoolease/crashes/ so the
// user can review what was happening immediately before the crash.
function CrashLogsSection() {
  const { data, isLoading, isError, refetch, isFetching } = useCrashes();
  const del   = useDeleteCrash();
  const clear = useDeleteAllCrashes();

  const [viewing, setViewing] = useState<CrashEntry | null>(null);
  const [text, setText]       = useState<string>('');
  const [textErr, setTextErr] = useState<string | null>(null);
  const [textLoad, setTextLoad] = useState(false);

  const open = useCallback(async (e: CrashEntry) => {
    setViewing(e);
    setText('');
    setTextErr(null);
    setTextLoad(true);
    try {
      const t = await fetchCrashText(e.seq);
      setText(t);
    } catch (err) {
      setTextErr(err instanceof Error ? err.message : String(err));
    } finally {
      setTextLoad(false);
    }
  }, []);

  const close = useCallback(() => {
    setViewing(null);
    setText('');
    setTextErr(null);
  }, []);

  const downloadCurrent = async (seq: number) => {
    // Use a transient anchor so the browser saves with a friendly name.
    const r = await fetch(`/api/crashes/${seq}`);
    if (!r.ok) return;
    const blob = await r.blob();
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href = url;
    a.download = `spoolease-crash-${seq}.log`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  };

  const list = data?.crashes ?? [];

  return (
    <div className="py-2 border-b border-surface-border space-y-2">
      <div className="flex items-center gap-2">
        <FileWarning size={14} className="text-text-muted" />
        <div className="text-sm text-text-primary">Crash logs</div>
        <Button
          variant="secondary"
          onClick={() => refetch()}
          disabled={isFetching}
          className="!py-0.5 !px-1.5 ml-auto"
          title="Refresh list"
        >
          <RefreshCw size={11} className={isFetching ? 'animate-spin' : ''} />
        </Button>
      </div>
      <div className="text-xs text-text-muted">
        The firmware spills its in-memory log to the SD card every few
        seconds. After a panic / watchdog / brownout reset, the
        previous session's log is preserved here so you can review what
        was running just before the crash.
      </div>

      {isLoading && (
        <div className="text-xs text-text-muted italic">loading…</div>
      )}
      {isError && (
        <div className="text-xs text-red-400">
          Failed to load crash log list.
        </div>
      )}
      {!isLoading && data && !data.available && (
        <div className="rounded bg-surface-input border border-surface-border px-2 py-1.5 text-xs text-text-muted">
          Crash log persistence is unavailable — no SD card is mounted.
        </div>
      )}
      {data?.available && list.length === 0 && (
        <div className="rounded border border-dashed border-surface-border px-2 py-3 text-center text-xs text-text-muted italic">
          No crashes recorded. The console hasn't reset uncleanly since the
          card was last cleared.
        </div>
      )}
      {data?.available && list.length > 0 && (
        <>
          <div className="rounded border border-surface-border overflow-hidden">
            <table className="w-full text-xs">
              <thead className="bg-surface-input text-text-muted">
                <tr>
                  <th className="px-2 py-1 text-left font-normal">#</th>
                  <th className="px-2 py-1 text-left font-normal">Reason</th>
                  <th className="px-2 py-1 text-right font-normal">Size</th>
                  <th className="px-2 py-1 text-left font-normal">When</th>
                  <th className="px-2 py-1 text-right font-normal" />
                </tr>
              </thead>
              <tbody>
                {[...list].reverse().map((c) => (
                  <tr key={c.seq} className="border-t border-surface-border/60">
                    <td className="px-2 py-1 font-mono text-text-primary">{c.seq}</td>
                    <td className="px-2 py-1 font-mono">
                      <ReasonBadge reason={c.reason} />
                    </td>
                    <td className="px-2 py-1 text-right tabular-nums text-text-muted">
                      {(c.bytes / 1024).toFixed(1)} KB
                    </td>
                    <td className="px-2 py-1 text-text-muted">
                      {c.mtime > 0
                        ? new Date(c.mtime * 1000).toLocaleString()
                        : <span className="italic">unknown</span>}
                    </td>
                    <td className="px-2 py-1 text-right">
                      <div className="inline-flex gap-1">
                        <Button
                          variant="secondary"
                          onClick={() => open(c)}
                          className="!py-0.5 !px-1.5"
                          title="View"
                        >
                          <FolderOpen size={11} />
                        </Button>
                        <Button
                          variant="secondary"
                          onClick={() => downloadCurrent(c.seq)}
                          className="!py-0.5 !px-1.5"
                          title="Download"
                        >
                          <Download size={11} />
                        </Button>
                        <Button
                          variant="secondary"
                          onClick={() => del.mutate(c.seq)}
                          disabled={del.isPending}
                          className="!py-0.5 !px-1.5"
                          title="Delete"
                        >
                          <Trash2 size={11} />
                        </Button>
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="flex justify-end">
            <Button
              variant="secondary"
              onClick={() => {
                if (window.confirm(`Delete all ${list.length} crash log(s)?`)) {
                  clear.mutate();
                }
              }}
              disabled={clear.isPending}
              className="!py-0.5 !px-1.5"
            >
              <Trash2 size={11} className="mr-1 inline" />
              Clear all
            </Button>
          </div>
        </>
      )}

      {viewing && (
        <CrashLogModal
          entry={viewing}
          text={text}
          loading={textLoad}
          error={textErr}
          onClose={close}
        />
      )}
    </div>
  );
}

function ReasonBadge({ reason }: { reason: string }) {
  const tone =
    reason === 'panic'      ? 'bg-red-500/10 text-red-300 border-red-500/30' :
    reason === 'task_wdt'   ? 'bg-orange-500/10 text-orange-300 border-orange-500/30' :
    reason === 'int_wdt'    ? 'bg-orange-500/10 text-orange-300 border-orange-500/30' :
    reason === 'wdt'        ? 'bg-orange-500/10 text-orange-300 border-orange-500/30' :
    reason === 'brownout'   ? 'bg-yellow-500/10 text-yellow-300 border-yellow-500/30' :
                              'bg-surface-input text-text-muted border-surface-border';
  return (
    <span className={`inline-block px-1.5 py-0.5 rounded border text-[10px] ${tone}`}>
      {reason}
    </span>
  );
}

function CrashLogModal({
  entry, text, loading, error, onClose,
}: {
  entry:   CrashEntry;
  text:    string;
  loading: boolean;
  error:   string | null;
  onClose: () => void;
}) {
  const copy = useCallback(() => {
    navigator.clipboard?.writeText(text).catch(() => { /* clipboard blocked */ });
  }, [text]);

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4"
      onClick={onClose}
    >
      <div
        className="bg-surface-card border border-surface-border rounded-md w-full max-w-4xl max-h-[80vh] flex flex-col"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center gap-2 px-3 py-2 border-b border-surface-border">
          <FileWarning size={14} className="text-text-muted" />
          <div className="text-sm text-text-primary">
            Crash <span className="font-mono">#{entry.seq}</span>
          </div>
          <ReasonBadge reason={entry.reason} />
          <span className="text-xs text-text-muted">
            {(entry.bytes / 1024).toFixed(1)} KB
          </span>
          <div className="ml-auto flex items-center gap-1.5">
            <Button
              variant="secondary"
              onClick={copy}
              disabled={loading || !!error}
              className="!py-0.5 !px-1.5"
            >
              <Copy size={11} className="mr-1 inline" />
              Copy
            </Button>
            <Button
              variant="secondary"
              onClick={onClose}
              className="!py-0.5 !px-1.5"
            >
              Close
            </Button>
          </div>
        </div>
        <div className="flex-1 overflow-auto p-3 bg-surface-input">
          {loading && (
            <div className="text-xs text-text-muted italic">
              <Loader2 size={12} className="inline animate-spin mr-1" />
              loading…
            </div>
          )}
          {error && (
            <div className="text-xs text-red-400">
              Failed to load crash text: {error}
            </div>
          )}
          {!loading && !error && (
            <pre className="font-mono text-[11px] text-text-primary whitespace-pre-wrap break-all">
              {text || <span className="text-text-muted italic">(empty)</span>}
            </pre>
          )}
        </div>
      </div>
    </div>
  );
}

// Spool-storage backend toggle. The spool JSONL is the only frequently
// rewritten file on internal flash, so steering it to the SD card avoids
// userfs wear during long prints. UI shows current backend + SD presence
// + any startup error (e.g. "configured for SD but card missing"), and
// offers a one-click migrate-and-switch.
type SpoolStorageStatus = {
  configured: 'sd' | 'internal' | 'auto';
  backend:    'sd' | 'internal' | 'none';
  path?:      string;
  ready:      boolean;
  error:      string;
  sd_mounted: boolean;
  count?:     number;
};

function SpoolStorageBackend() {
  const [status, setStatus] = useState<SpoolStorageStatus | null>(null);
  const [busy, setBusy]     = useState(false);
  const [msg, setMsg]       = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      const r = await fetch('/api/storage/spools');
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      setStatus(await r.json());
    } catch (e) {
      setStatus(null);
    }
  }, []);
  useEffect(() => { refresh(); }, [refresh]);

  const set = async (target: 'sd' | 'internal', migrate: boolean) => {
    setBusy(true); setMsg(null);
    try {
      const r = await fetch('/api/storage/spools', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ target, migrate }),
      });
      const payload = await r.json().catch(() => ({}));
      if (!r.ok) {
        setMsg(payload.error ?? `HTTP ${r.status}`);
      } else {
        setMsg(payload.message ?? 'ok');
      }
      await refresh();
    } catch (e) {
      setMsg(String(e));
    } finally {
      setBusy(false);
    }
  };

  if (!status) {
    return (
      <div className="py-2 border-b border-surface-border text-xs text-text-muted">
        loading storage status…
      </div>
    );
  }

  const onSd        = status.backend === 'sd';
  const onInternal  = status.backend === 'internal';
  const noBackend   = status.backend === 'none';
  const wantSd      = status.configured === 'sd';

  return (
    <div className="py-2 border-b border-surface-border space-y-2">
      <div className="flex items-center gap-2">
        <Database size={14} className="text-text-muted" />
        <div className="text-sm text-text-primary">Spool storage backend</div>
      </div>
      <div className="text-xs text-text-muted">
        The spool database is the only file the firmware rewrites frequently
        (every 5% of every print). Moving it to the SD card avoids wearing
        the ESP32's internal flash. NVS settings (Wi-Fi, printer config,
        cloud token) and the read-only filaments library are unaffected.
      </div>

      {/* Status row */}
      <div className="flex items-center gap-2 text-xs flex-wrap">
        <span className="text-text-muted">Currently:</span>
        {onSd && <span className="inline-flex items-center gap-1 text-green-400 font-mono"><HardDrive size={12} /> SD</span>}
        {onInternal && <span className="inline-flex items-center gap-1 text-text-primary font-mono"><Database size={12} /> internal</span>}
        {noBackend && <span className="inline-flex items-center gap-1 text-red-400 font-mono"><AlertTriangle size={12} /> unavailable</span>}
        {status.path && <span className="font-mono text-text-muted">{status.path}</span>}
        {typeof status.count === 'number' && status.ready && (
          <span className="text-text-muted ml-auto">{status.count} record{status.count === 1 ? '' : 's'}</span>
        )}
      </div>

      {/* SD presence */}
      <div className="text-[11px] text-text-muted font-mono">
        SD card {status.sd_mounted ? 'mounted' : 'not mounted'}
        {' · '}preference saved as {status.configured}
      </div>

      {/* Error surface — typically "configured for SD but no card" */}
      {!status.ready && status.error && (
        <div className="rounded bg-red-500/10 border border-red-500/30 p-2 text-xs text-red-300">
          <div className="flex items-start gap-2">
            <AlertTriangle size={14} className="shrink-0 mt-0.5" />
            <div>
              <div className="font-medium mb-1">Spool tracker not running</div>
              <div className="text-red-200">{status.error}</div>
            </div>
          </div>
          <div className="mt-2 flex flex-wrap gap-1.5">
            <Button
              variant="secondary"
              onClick={() => set('internal', false)}
              disabled={busy}
              className="!py-1 !px-2"
              title="Override to internal flash. Spool DB starts empty until you reboot or insert the SD card."
            >
              Override → internal flash
            </Button>
          </div>
        </div>
      )}

      {/* Action buttons */}
      <div className="flex items-center gap-1.5 flex-wrap">
        <Button
          variant="secondary"
          onClick={() => set('sd', true)}
          disabled={busy || onSd || !status.sd_mounted}
          className="!py-1 !px-2"
          title={status.sd_mounted ? 'Copy spools.jsonl to /spoolease/ on the SD card and switch the live store to it.' : 'Insert an SD card first'}
        >
          <HardDrive size={12} className="mr-1 inline" />
          Move to SD
        </Button>
        <Button
          variant="secondary"
          onClick={() => set('internal', true)}
          disabled={busy || onInternal}
          className="!py-1 !px-2"
          title="Copy spools.jsonl back to internal flash and switch the live store to it."
        >
          <Database size={12} className="mr-1 inline" />
          Move to internal
        </Button>
        {wantSd && !status.sd_mounted && (
          <Button
            variant="secondary"
            onClick={() => set('internal', false)}
            disabled={busy}
            className="!py-1 !px-2"
            title="Stop trying to use SD; on next reboot, run from internal flash."
          >
            Stop trying SD
          </Button>
        )}
        {busy && (
          <span className="inline-flex items-center gap-1 text-xs text-text-muted">
            <Loader2 size={12} className="animate-spin" /> working…
          </span>
        )}
        {msg && !busy && (
          <span className="text-xs text-text-muted ml-auto">{msg}</span>
        )}
      </div>
    </div>
  );
}

// Interactive FTPS debug — runs probe / list / download against a connected
// printer's FTPS daemon and streams per-step progress over the WebSocket
// (`ftp_trace` + `ftp_done` debug events emitted by
// BambuPrinter::_runFtpDebug). Renders every step live with code, text,
// and elapsed_ms so you can see exactly where the conversation stalls.
function FtpAnalysisProbe() {
  const { data: printers } = usePrinters();
  const connected = (printers ?? []).filter((p) => p.state?.link === 'connected');

  return (
    <div className="py-2 border-b border-surface-border">
      <div className="flex items-center gap-2 mb-2">
        <Zap size={14} className="text-text-muted" />
        <div className="text-sm text-text-primary">FTP debug</div>
      </div>
      <div className="text-xs text-text-muted mb-3">
        Talks directly to the printer's FTPS (port 990) so you can watch each
        control-channel exchange. <b>Probe</b> stops after login; <b>List</b>
        sends PASV+LIST and shows the entries; <b>Download</b> streams a
        file to <span className="font-mono">SD:/ftp_dl.bin</span> and links
        it for the browser. The "Analyze current print" action still lives in
        the Dashboard's Printers panel.
      </div>
      {connected.length === 0 ? (
        <div className="text-xs text-text-muted italic">No printers currently connected.</div>
      ) : (
        <div className="space-y-2">
          {connected.map((p) => <FtpDebugRow key={p.serial} p={p} />)}
        </div>
      )}
    </div>
  );
}

function FtpDebugRow({ p }: { p: Printer }) {
  const [path, setPath]     = useState('/cache');
  const [op, setOp]         = useState<'' | 'probe' | 'list' | 'download'>('');
  const [trace, setTrace]   = useState<FtpTraceLine[]>([]);
  const [done, setDone]     = useState<FtpDoneLine | null>(null);
  const [error, setError]   = useState<string | null>(null);
  const [quirkMsg, setQuirkMsg] = useState<string | null>(null);

  const running = op !== '' && !done;

  // Force the firmware to re-probe whether this printer's FTPS daemon
  // accepts data-channel session reuse. Use after a printer firmware
  // update if the cached preference is stale and the self-healing
  // fallback's 15 s extra round-trip is annoying.
  const resetQuirks = async () => {
    setQuirkMsg(null);
    try {
      const r = await fetch(`/api/printers/${encodeURIComponent(p.serial)}/ftp-quirks/reset`, {
        method: 'POST',
      });
      const payload = await r.json().catch(() => ({}));
      if (!r.ok) {
        setQuirkMsg(payload.error ?? `HTTP ${r.status}`);
        return;
      }
      setQuirkMsg(payload.existed ? 'cleared' : 'no entry');
      setTimeout(() => setQuirkMsg(null), 3000);
    } catch (e) {
      setQuirkMsg(String(e));
    }
  };

  const start = async (nextOp: 'probe' | 'list' | 'download') => {
    setTrace([]);
    setDone(null);
    setError(null);
    setOp(nextOp);
    try {
      const body: Record<string, string> = { op: nextOp };
      if (nextOp !== 'probe') body.path = path;
      // POST with a chunked NDJSON response — we read the body as a
      // ReadableStream so trace lines show up progressively while the
      // firmware is still mid-conversation with the printer.
      const r = await fetch(`/api/printers/${encodeURIComponent(p.serial)}/ftp-debug`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!r.ok) {
        const payload = await r.json().catch(() => ({}));
        setError(payload.error ?? `HTTP ${r.status}`);
        setOp('');
        return;
      }
      if (!r.body) {
        setError('no response body');
        setOp('');
        return;
      }
      const reader  = r.body.getReader();
      const decoder = new TextDecoder();
      let buffer    = '';
      while (true) {
        const { value, done: streamDone } = await reader.read();
        if (value) buffer += decoder.decode(value, { stream: true });
        // Parse complete lines; keep any trailing partial for next chunk.
        let nl: number;
        while ((nl = buffer.indexOf('\n')) >= 0) {
          const line = buffer.slice(0, nl).trim();
          buffer = buffer.slice(nl + 1);
          if (!line) continue;
          try {
            const ev = JSON.parse(line) as FtpStreamLine;
            if (ev.kind === 'trace')      setTrace((prev) => [...prev, ev]);
            else if (ev.kind === 'done')  setDone(ev);
          } catch {
            // Silently skip malformed lines — an OOM truncation at the
            // very end of the stream is the main realistic source.
          }
        }
        if (streamDone) break;
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setOp('');
    }
  };

  return (
    <div className="rounded border border-surface-border bg-surface-card">
      <div className="p-2 space-y-2">
        {/* Header row: printer identity + path input */}
        <div className="flex items-center gap-2">
          <div className="flex-1 min-w-0">
            <div className="text-sm text-text-primary truncate">{p.name || p.serial}</div>
            <div className="text-[11px] text-text-muted font-mono truncate">{p.ip} · {p.serial}</div>
          </div>
          <input
            type="text"
            value={path}
            onChange={(e) => setPath(e.target.value)}
            placeholder="/cache"
            disabled={running}
            className="px-2 py-1 text-xs font-mono bg-surface-input border border-surface-border rounded w-48 text-text-primary"
          />
        </div>
        {/* Buttons */}
        <div className="flex items-center gap-1.5 flex-wrap">
          <Button variant="secondary" onClick={() => start('probe')} disabled={running} className="!py-1 !px-2">
            <Zap size={13} className="mr-1 inline" />Probe
          </Button>
          <Button variant="secondary" onClick={() => start('list')} disabled={running} className="!py-1 !px-2">
            <FolderOpen size={13} className="mr-1 inline" />List
          </Button>
          <Button variant="secondary" onClick={() => start('download')} disabled={running} className="!py-1 !px-2">
            <Download size={13} className="mr-1 inline" />Download
          </Button>
          <Button
            variant="secondary"
            onClick={resetQuirks}
            disabled={running}
            className="!py-1 !px-2"
            title="Forget the cached TLS-session-reuse preference for this printer (re-probes on the next FTPS open)"
          >
            <Eraser size={13} className="mr-1 inline" />Reset quirks
          </Button>
          {quirkMsg && (
            <span className="inline-flex items-center gap-1 text-xs text-text-muted">
              {quirkMsg}
            </span>
          )}
          {running && (
            <span className="inline-flex items-center gap-1 text-xs text-text-muted ml-auto">
              <Loader2 size={12} className="animate-spin" />
              {op}…
            </span>
          )}
          {done && (
            <span
              className={`inline-flex items-center gap-1 text-xs ml-auto ${done.ok ? 'text-green-400' : 'text-red-400'}`}
              title={done.message}
            >
              {done.ok ? <CheckCircle2 size={12} /> : <XCircle size={12} />}
              <span className="truncate max-w-[320px]">{done.message}</span>
            </span>
          )}
          {error && (
            <span className="inline-flex items-center gap-1 text-xs text-red-400 ml-auto">
              <XCircle size={12} />
              {error}
            </span>
          )}
        </div>
        {/* Trace */}
        {trace.length > 0 && <TraceList trace={trace} />}
        {/* Op-specific result views */}
        {done?.ok && done.op === 'list' && done.payload?.entries && (
          <ListResult entries={done.payload.entries} />
        )}
        {done?.ok && done.op === 'download' && done.payload?.url && (
          <DownloadResult bytes={done.payload.bytes ?? 0} url={done.payload.url} filename={done.payload.path ?? ''} />
        )}
      </div>
    </div>
  );
}

function TraceList({ trace }: { trace: FtpTraceLine[] }) {
  return (
    <div className="rounded border border-surface-border bg-surface-input max-h-64 overflow-y-auto font-mono text-[11px]">
      <table className="w-full border-collapse">
        <tbody>
          {trace.map((t, i) => (
            <tr key={i} className="border-t border-surface-border/60 first:border-t-0">
              <td className="px-2 py-1 text-text-muted tabular-nums w-14">{t.elapsed_ms}ms</td>
              <td className="px-2 py-1 text-brand-400 whitespace-nowrap">{t.step}</td>
              <td className={`px-2 py-1 tabular-nums w-10 text-right ${codeToneClass(t.code)}`}>
                {t.code || ''}
              </td>
              <td className="px-2 py-1 text-text-primary break-all">{t.text}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function codeToneClass(code: number): string {
  if (code <= 0)   return 'text-text-muted';
  if (code < 300)  return 'text-green-400';    // 1xx/2xx FTP = ok
  if (code < 400)  return 'text-cyan-400';     // 3xx = more info needed (331)
  return 'text-red-400';                        // 4xx/5xx = error
}

function ListResult({ entries }: { entries: string[] }) {
  if (entries.length === 0) {
    return <div className="text-xs text-text-muted italic px-1">(empty directory)</div>;
  }
  return (
    <div className="rounded border border-surface-border bg-surface-input max-h-64 overflow-y-auto">
      <ul className="text-[11px] font-mono divide-y divide-surface-border/60">
        {entries.map((e, i) => (
          <li key={i} className="px-2 py-1 text-text-primary break-all">{e}</li>
        ))}
      </ul>
    </div>
  );
}

function DownloadResult({ bytes, url, filename }: { bytes: number; url: string; filename: string }) {
  const kb = (bytes / 1024).toFixed(1);
  return (
    <div className="rounded border border-green-500/30 bg-green-500/10 px-2 py-1.5 text-xs">
      <span className="text-green-300">Downloaded {kb} KiB</span>{' '}
      <span className="text-text-muted font-mono">{filename}</span>{' '}
      <a href={url} download className="ml-2 inline-flex items-center gap-1 text-brand-400 underline">
        <Download size={12} />
        save to disk
      </a>
    </div>
  );
}

function Toggle({ on, disabled, onClick }: { on: boolean; disabled?: boolean; onClick: () => void }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      disabled={disabled}
      onClick={onClick}
      className={`relative inline-flex h-6 w-11 items-center rounded-full transition-colors ${
        on ? 'bg-brand-500' : 'bg-surface-input border border-surface-border'
      } ${disabled ? 'opacity-50 cursor-not-allowed' : 'cursor-pointer'}`}
    >
      <span
        className={`inline-block h-4 w-4 transform rounded-full bg-white transition ${
          on ? 'translate-x-6' : 'translate-x-1'
        }`}
      />
    </button>
  );
}
