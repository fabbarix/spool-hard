import { useRef, useState } from 'react';
import { Save, Upload, AlertTriangle, ShieldAlert, Download } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { useReconnect } from '@spoolhard/ui/providers/ReconnectProvider';
import { getStoredKey } from '@spoolhard/ui/utils/authStorage';

// Download/upload UI for the firmware's /api/backup + /api/restore
// endpoints. The backup is a single JSON document carrying every NVS
// namespace + filesystem file the device owns; restore parses an
// upload of the same shape and writes it back, then reboots.
//
// SECURITY: the file contains every secret the device knows — WiFi
// password, fixed bearer key, Bambu Cloud token, scale-link shared
// secrets, printer access codes. We force the user through an
// explicit confirmation before generating a download so they can't
// click through it without seeing the warning.
export function BackupRestoreSection() {
  const [downloading,    setDownloading]    = useState(false);
  const [downloadError,  setDownloadError]  = useState<string | null>(null);
  const [acknowledged,   setAcknowledged]   = useState(false);

  const [restoreInfo,    setRestoreInfo]    = useState<RestoreReport | null>(null);
  const [restoring,      setRestoring]      = useState(false);
  const [restoreError,   setRestoreError]   = useState<string | null>(null);
  const [pendingFile,    setPendingFile]    = useState<File | null>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  const { start: startReconnect } = useReconnect();

  const headers = (): HeadersInit => {
    const k = getStoredKey();
    return k ? { Authorization: `Bearer ${k}` } : {};
  };

  const downloadBackup = async () => {
    setDownloadError(null);
    setDownloading(true);
    try {
      const r = await fetch('/api/backup', { headers: headers() });
      if (!r.ok) throw new Error(`backup failed: HTTP ${r.status}`);
      const blob = await r.blob();
      // Pull the suggested filename out of Content-Disposition. Falls
      // back to a date-stamped default if the header isn't there.
      const cd = r.headers.get('Content-Disposition') || '';
      const m  = cd.match(/filename="([^"]+)"/);
      const name = m
        ? m[1]
        : `spoolhard-scale-${new Date().toISOString().slice(0, 10)}-backup.json`;
      const url = URL.createObjectURL(blob);
      const a   = document.createElement('a');
      a.href = url; a.download = name;
      document.body.appendChild(a); a.click(); a.remove();
      URL.revokeObjectURL(url);
    } catch (e) {
      setDownloadError((e as Error)?.message ?? 'download failed');
    } finally {
      setDownloading(false);
    }
  };

  const stageFile = (f: File) => {
    setPendingFile(f);
    setRestoreInfo(null);
    setRestoreError(null);
  };

  const restore = async () => {
    if (!pendingFile) return;
    setRestoring(true);
    setRestoreError(null);
    setRestoreInfo(null);
    try {
      // Raw POST body (not multipart). The firmware route accumulates
      // chunks via the body-handler third arg of _server.on(...).
      const r = await fetch('/api/restore', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...headers() },
        body: pendingFile,
      });
      // The device reboots ~1s after sending the response on success.
      // The fetch promise may resolve before the disconnect; we still
      // want to show the report and trigger the reconnect overlay.
      const j = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(j?.error ?? `restore failed: HTTP ${r.status}`);
      setRestoreInfo({
        nvs_keys_set:     j.nvs_keys_set     ?? 0,
        nvs_keys_skipped: j.nvs_keys_skipped ?? 0,
        files_written:    j.files_written    ?? 0,
        files_skipped:    j.files_skipped    ?? 0,
        errors:           j.errors           ?? 0,
        first_error:      j.first_error,
      });
      // Anything actually applied → device is rebooting. Pop the
      // reconnect overlay so the user sees a clear "wait for boot"
      // state instead of a frozen page.
      if ((j.nvs_keys_set || j.files_written) && j.errors === 0) {
        startReconnect('Restoring backup — device rebooting…');
      }
      setPendingFile(null);
      if (fileInput.current) fileInput.current.value = '';
    } catch (e) {
      setRestoreError((e as Error)?.message ?? 'restore failed');
    } finally {
      setRestoring(false);
    }
  };

  return (
    <SectionCard
      title="Backup & Restore"
      icon={<Save size={16} />}
      description="Download a single JSON file capturing every configuration namespace and on-disk database the scale owns. Re-upload the same file (on this or a sibling device) to restore the full state. The device reboots automatically after a successful restore."
    >
      {/* ── Download ─────────────────────────────────────────── */}
      <div className="space-y-3">
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs text-amber-300">
          <div className="flex items-center gap-2 font-semibold">
            <ShieldAlert size={14} /> The backup contains every secret on the device
          </div>
          <div className="mt-1 text-amber-200/80">
            Including the WiFi password, security key, Bambu Cloud token, scale-link
            shared secret, and printer access codes. Treat the file like a password
            vault — anyone with it can fully impersonate the device.
          </div>
          <label className="mt-2 flex items-center gap-2 cursor-pointer select-none">
            <input
              type="checkbox"
              checked={acknowledged}
              onChange={(e) => setAcknowledged(e.target.checked)}
              className="accent-amber-500"
            />
            <span>I understand and will store this file securely.</span>
          </label>
        </div>

        <div className="flex items-center gap-2">
          <Button onClick={downloadBackup} disabled={!acknowledged || downloading}>
            <Download size={14} className="mr-1.5 inline" />
            {downloading ? 'Generating…' : 'Download backup'}
          </Button>
          {downloadError && (
            <span className="text-xs text-red-400 flex items-center gap-1">
              <AlertTriangle size={12} /> {downloadError}
            </span>
          )}
        </div>
      </div>

      {/* ── Restore ──────────────────────────────────────────── */}
      <div className="border-t border-surface-border pt-4 mt-4 space-y-3">
        <div className="text-xs font-semibold uppercase tracking-wider text-text-secondary">
          Restore from a backup file
        </div>
        <div className="text-xs text-text-muted">
          Restoring overwrites the matching configuration namespaces and files on
          this device. Anything not present in the backup is left alone — restore
          is additive, not a factory reset. The console reboots automatically when
          the apply finishes.
        </div>
        <div className="flex items-center gap-2">
          <input
            ref={fileInput}
            type="file"
            accept="application/json,.json"
            onChange={(e) => {
              const f = e.target.files?.[0];
              if (f) stageFile(f);
            }}
            className="text-xs text-text-muted file:mr-2 file:rounded-md file:border-0 file:bg-surface-input file:px-3 file:py-1.5 file:text-xs file:text-text-primary hover:file:bg-surface-card-hover cursor-pointer"
          />
          <Button onClick={restore} disabled={!pendingFile || restoring}>
            <Upload size={14} className="mr-1.5 inline" />
            {restoring ? 'Restoring…' : 'Restore now'}
          </Button>
        </div>
        {pendingFile && !restoring && !restoreInfo && (
          <div className="text-[11px] text-text-muted font-mono">
            Ready: {pendingFile.name} ({(pendingFile.size / 1024).toFixed(1)} KB)
          </div>
        )}
        {restoreError && (
          <div className="rounded-md border border-red-500/30 bg-red-500/10 p-2 text-xs text-red-300 break-all">
            {restoreError}
          </div>
        )}
        {restoreInfo && (
          <div className={`rounded-md border p-2 text-xs ${
            restoreInfo.errors > 0
              ? 'border-amber-500/30 bg-amber-500/10 text-amber-300'
              : 'border-teal-500/30 bg-teal-500/10 text-teal-300'
          }`}>
            <div>
              Restored {restoreInfo.nvs_keys_set} NVS keys
              {restoreInfo.nvs_keys_skipped > 0 && ` (skipped ${restoreInfo.nvs_keys_skipped})`}
              {' '}and {restoreInfo.files_written} files
              {restoreInfo.files_skipped > 0 && ` (skipped ${restoreInfo.files_skipped})`}.
              {restoreInfo.errors > 0 && (
                <> {restoreInfo.errors} errors{restoreInfo.first_error ? ` — first: ${restoreInfo.first_error}` : ''}.</>
              )}
            </div>
            {!restoreInfo.errors && (restoreInfo.nvs_keys_set || restoreInfo.files_written) && (
              <div className="mt-1 text-text-muted">Device rebooting…</div>
            )}
          </div>
        )}
      </div>
    </SectionCard>
  );
}

interface RestoreReport {
  nvs_keys_set: number;
  nvs_keys_skipped: number;
  files_written: number;
  files_skipped: number;
  errors: number;
  first_error?: string;
}
