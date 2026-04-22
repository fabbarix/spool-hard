import type { ReactNode } from 'react';

export interface SubTab<T extends string> {
  id: T;
  label: string;
  icon?: ReactNode;
}

interface SubTabBarProps<T extends string> {
  tabs: SubTab<T>[];
  active: T;
  onChange: (id: T) => void;
}

/**
 * Pill-style tab bar rendered inside a surface-card container. Used for
 * secondary navigation on config/settings pages across SpoolHard products.
 */
export function SubTabBar<T extends string>({ tabs, active, onChange }: SubTabBarProps<T>) {
  return (
    <div className="flex gap-1 rounded-card bg-surface-card border border-surface-border p-1">
      {tabs.map((tab) => (
        <button
          key={tab.id}
          onClick={() => onChange(tab.id)}
          className={`
            flex items-center gap-1.5 px-4 py-2 rounded-[7px] text-sm transition-all duration-200 cursor-pointer
            ${active === tab.id
              ? 'bg-brand-500/15 text-brand-400 font-medium shadow-sm'
              : 'text-text-secondary hover:text-text-primary hover:bg-surface-card-hover'}
          `}
        >
          {tab.icon}
          {tab.label}
        </button>
      ))}
    </div>
  );
}
