import { RotateCcw, CheckCircle, AlertTriangle } from 'lucide-react';
import { useReconnect } from '../providers/ReconnectProvider';

// Full-screen modal shown while we wait for the device to reboot and come
// back online. Pairs with <ReconnectProvider>; mount one per app at the top
// level and the overlay auto-appears whenever any code calls `start()`.
export function ReconnectOverlay() {
  const { phase, message } = useReconnect();

  if (phase === 'idle') return null;

  const isBack   = phase === 'back';
  const isFailed = phase === 'timeout';

  return (
    <div
      className="fixed inset-0 z-[100] flex items-center justify-center bg-surface-body/85 backdrop-blur-sm animate-in fade-in"
      role="status"
      aria-live="polite"
    >
      <div className="flex flex-col items-center gap-3 rounded-card border border-surface-border bg-surface-card px-8 py-6 shadow-lg max-w-sm text-center">
        {isBack ? (
          <>
            <CheckCircle size={36} className="text-status-connected" />
            <div className="text-sm font-medium text-status-connected">Back online — reloading…</div>
          </>
        ) : isFailed ? (
          <>
            <AlertTriangle size={36} className="text-status-error" />
            <div className="text-sm font-medium text-status-error">Reconnect failed</div>
            <div className="text-xs text-text-secondary">{message}</div>
            <button
              onClick={() => window.location.reload()}
              className="mt-2 px-3 py-1.5 rounded-button text-sm font-medium bg-brand-500 text-surface-body hover:bg-brand-400"
            >
              Reload now
            </button>
          </>
        ) : (
          <>
            <RotateCcw size={36} className="text-brand-400 animate-spin" />
            <div className="text-sm font-medium text-text-primary">{message}</div>
            <div className="text-xs text-text-muted">
              {phase === 'waiting'
                ? 'Giving the device a moment to reboot…'
                : 'Waiting for it to come back online…'}
            </div>
          </>
        )}
      </div>
    </div>
  );
}
