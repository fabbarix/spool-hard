/** Pulsing placeholder block. Size it with className (h-*, w-*). */
export function Skeleton({ className = '' }: { className?: string }) {
  return <div className={`animate-pulse rounded bg-surface-border/70 ${className}`} aria-hidden="true" />;
}

/** N list-row placeholders (dot + two text lines + value). */
export function SkeletonRows({ rows = 4 }: { rows?: number }) {
  return (
    <div role="status" aria-label="Loading" className="space-y-2">
      {Array.from({ length: rows }, (_, i) => (
        <div key={i} className="flex items-center gap-3 rounded-md border border-surface-border bg-surface-input p-3">
          <Skeleton className="h-8 w-8 rounded-full shrink-0" />
          <div className="flex-1 space-y-2">
            <Skeleton className="h-3 w-1/2" />
            <Skeleton className="h-2.5 w-1/3" />
          </div>
          <Skeleton className="h-3.5 w-12" />
        </div>
      ))}
      <span className="sr-only">Loading…</span>
    </div>
  );
}
