import { useEffect, useId, useRef, useState, type ReactNode } from 'react';
import { AlertTriangle } from 'lucide-react';
import { Button } from './Button';

interface ConfirmDialogProps {
  open: boolean;
  title: string;
  message: ReactNode;
  confirmLabel?: string;
  cancelLabel?: string;
  /** Visual style of the confirm button. Defaults to 'danger'. */
  variant?: 'primary' | 'danger';
  /** When set, the confirm button stays disabled until the user types this exact text. */
  requireText?: string;
  busy?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
}

const FOCUSABLE = 'button:not([disabled]), input:not([disabled]), [href], select, textarea, [tabindex]:not([tabindex="-1"])';

/**
 * Accessible modal confirmation. Esc cancels, Tab is trapped inside, focus
 * moves into the dialog on open (cancel button, or the input for the
 * type-to-confirm variant) and is restored to the opener on close.
 */
export function ConfirmDialog({
  open, title, message, confirmLabel = 'Confirm', cancelLabel = 'Cancel',
  variant = 'danger', requireText, busy = false, onConfirm, onCancel,
}: ConfirmDialogProps) {
  const [typed, setTyped] = useState('');
  const ref = useRef<HTMLDivElement>(null);
  const cancelRef = useRef<HTMLButtonElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const titleId = useId();
  const descId = useId();
  const cancelCb = useRef(onCancel);
  cancelCb.current = onCancel;

  useEffect(() => {
    if (!open) return;
    setTyped('');
    const prev = document.activeElement as HTMLElement | null;
    (inputRef.current ?? cancelRef.current)?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        cancelCb.current();
      } else if (e.key === 'Tab' && ref.current) {
        const els = Array.from(ref.current.querySelectorAll<HTMLElement>(FOCUSABLE));
        if (els.length === 0) return;
        const first = els[0], last = els[els.length - 1];
        if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
        else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
      }
    };
    document.addEventListener('keydown', onKey, true);
    return () => {
      document.removeEventListener('keydown', onKey, true);
      prev?.focus?.();
    };
  }, [open]);

  if (!open) return null;
  const canConfirm = !busy && (!requireText || typed === requireText);

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4"
      onMouseDown={(e) => { if (e.target === e.currentTarget && !busy) onCancel(); }}
    >
      <div
        ref={ref}
        role={variant === 'danger' ? 'alertdialog' : 'dialog'}
        aria-modal="true"
        aria-labelledby={titleId}
        aria-describedby={descId}
        className="w-full max-w-md rounded-card border border-surface-border bg-surface-card p-5 shadow-xl space-y-4"
      >
        <div className="flex items-start gap-3">
          {variant === 'danger' && <AlertTriangle size={20} className="text-status-error shrink-0 mt-0.5" aria-hidden="true" />}
          <div className="space-y-1">
            <h2 id={titleId} className="text-sm font-semibold text-text-primary">{title}</h2>
            <div id={descId} className="text-xs text-text-secondary">{message}</div>
          </div>
        </div>
        {requireText && (
          <label className="block text-xs text-text-secondary space-y-1">
            <span>Type <code className="font-mono text-text-primary">{requireText}</code> to confirm</span>
            <input
              ref={inputRef}
              value={typed}
              onChange={(e) => setTyped(e.target.value)}
              onKeyDown={(e) => { if (e.key === 'Enter' && canConfirm) onConfirm(); }}
              autoComplete="off"
              autoCapitalize="off"
              spellCheck={false}
              className="w-full rounded-button border border-surface-border bg-surface-input px-3 py-2 text-sm font-mono text-text-primary"
            />
          </label>
        )}
        <div className="flex justify-end gap-2">
          <Button ref={cancelRef} variant="secondary" onClick={onCancel} disabled={busy}>{cancelLabel}</Button>
          <Button variant={variant} onClick={onConfirm} disabled={!canConfirm}>{confirmLabel}</Button>
        </div>
      </div>
    </div>
  );
}
