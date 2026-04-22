import type { CSSProperties, ReactNode } from 'react';

interface StatCardProps {
  label: string;
  value: string | ReactNode;
  /** If true, value renders in the data/brand accent color (JetBrains Mono, 2xl). Default true. */
  dataColor?: boolean;
  className?: string;
  style?: CSSProperties;
}

/**
 * Uniform metric card used across the SpoolHard dashboards. Keeps labels at
 * 10px uppercase tracking-widest and values in JetBrains Mono so numbers
 * line up between products.
 */
export function StatCard({ label, value, dataColor = true, className = '', style }: StatCardProps) {
  return (
    <div
      className={`animate-in bg-surface-card border border-surface-border rounded-card p-4 ${className}`}
      style={style}
    >
      <div className="text-[10px] uppercase tracking-widest text-text-muted font-medium mb-2">{label}</div>
      <div className={`font-data font-semibold tabular-nums ${dataColor ? 'text-2xl text-text-data' : 'text-lg text-text-primary'}`}>
        {value}
      </div>
    </div>
  );
}
