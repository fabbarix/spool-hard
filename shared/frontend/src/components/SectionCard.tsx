import type { ReactNode } from 'react';

interface SectionCardProps {
  title: string;
  icon?: ReactNode;
  description?: string;
  highlight?: boolean;
  children: ReactNode;
}

export function SectionCard({ title, icon, description, highlight, children }: SectionCardProps) {
  return (
    <div
      className={`rounded-card border bg-surface-card p-5 space-y-4 ${
        highlight
          ? 'border-brand-500/50 shadow-[0_0_15px_rgba(240,180,41,0.05)]'
          : 'border-surface-border'
      }`}
    >
      <div>
        <div className="flex items-center gap-2">
          {icon && <span className="text-text-muted">{icon}</span>}
          <h3 className="text-sm font-semibold uppercase tracking-wider text-text-secondary">{title}</h3>
        </div>
        {description && (
          <p className="mt-1 text-xs text-text-muted leading-relaxed">{description}</p>
        )}
      </div>
      <div className="space-y-4">{children}</div>
    </div>
  );
}
