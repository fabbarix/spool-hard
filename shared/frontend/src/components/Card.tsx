import type { ReactNode } from 'react';

interface CardProps {
  title: string;
  actions?: ReactNode;
  children: ReactNode;
  accentColor?: string;
}

export function Card({ title, actions, children, accentColor }: CardProps) {
  return (
    <div
      className="rounded-card border border-surface-border bg-surface-card overflow-hidden"
      style={accentColor ? { borderLeftWidth: '2px', borderLeftColor: accentColor } : undefined}
    >
      <div className="flex items-center justify-between border-b border-surface-border px-4 py-3">
        <h3 className="text-xs font-semibold uppercase tracking-wider text-text-secondary">{title}</h3>
        {actions && <div className="flex items-center gap-2">{actions}</div>}
      </div>
      <div className="p-4">{children}</div>
    </div>
  );
}
