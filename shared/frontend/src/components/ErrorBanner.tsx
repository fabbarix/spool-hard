import { AlertTriangle } from 'lucide-react';
import { Button } from './Button';

interface ErrorBannerProps {
  /** What failed, e.g. "Could not load spools". */
  title: string;
  /** Optional detail (error message). */
  detail?: string;
  onRetry?: () => void;
  retrying?: boolean;
}

/** Inline error banner with a retry button, for failed fetches. */
export function ErrorBanner({ title, detail, onRetry, retrying }: ErrorBannerProps) {
  return (
    <div
      role="alert"
      className="flex flex-wrap items-center gap-3 rounded-card border border-status-error/40 bg-status-error/10 px-4 py-3"
    >
      <AlertTriangle size={18} className="text-status-error shrink-0" aria-hidden="true" />
      <div className="min-w-0 flex-1 basis-48">
        <div className="text-sm font-medium text-text-primary">{title}</div>
        {detail && <div className="text-xs text-text-secondary break-words">{detail}</div>}
      </div>
      {onRetry && (
        <Button variant="secondary" onClick={onRetry} disabled={retrying}>
          {retrying ? 'Retrying…' : 'Retry'}
        </Button>
      )}
    </div>
  );
}
