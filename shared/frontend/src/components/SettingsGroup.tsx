import type { ReactNode } from 'react';
import { ChevronDown } from 'lucide-react';

interface SettingsGroupProps {
  title: string;
  description?: string;
  /** Start expanded. Defaults to true. */
  defaultOpen?: boolean;
  /** Red accent for destructive groups. */
  danger?: boolean;
  children: ReactNode;
}

/**
 * Collapsible group of settings cards (native <details>, so keyboard and
 * screen-reader support is free). Gives long Config tabs the same
 * structure on console and scale.
 */
export function SettingsGroup({ title, description, defaultOpen = true, danger, children }: SettingsGroupProps) {
  return (
    <details open={defaultOpen} className="group space-y-4">
      <summary className="flex min-h-11 cursor-pointer list-none items-center gap-2 rounded-md px-1 [&::-webkit-details-marker]:hidden">
        <ChevronDown size={16} className="text-text-muted transition-transform -rotate-90 group-open:rotate-0" aria-hidden="true" />
        <span className={`text-xs font-semibold uppercase tracking-wider ${danger ? 'text-status-error' : 'text-text-secondary'}`}>
          {title}
        </span>
        {description && <span className="hidden text-xs text-text-muted sm:inline">{description}</span>}
      </summary>
      <div className="space-y-4">{children}</div>
    </details>
  );
}
