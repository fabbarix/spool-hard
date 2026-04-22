import { LayoutDashboard, Settings, Package } from 'lucide-react';
import type { Tab } from '../../App';

interface NavTabsProps {
  activeTab: Tab;
  onTabChange: (tab: Tab) => void;
}

const tabs = [
  { key: 'dashboard' as const, label: 'Dashboard', icon: LayoutDashboard },
  { key: 'spools'    as const, label: 'Spools',    icon: Package },
  { key: 'config'    as const, label: 'Config',    icon: Settings },
];

export function NavTabs({ activeTab, onTabChange }: NavTabsProps) {
  return (
    // Same trick as <Header>: full-width strip, inner row capped at the
    // content max-width so the tabs line up with the cards below.
    <nav className="bg-surface-header border-b border-surface-border">
      <div className="mx-auto max-w-[1100px] px-4 flex">
        {tabs.map((t) => {
          const Icon = t.icon;
          const active = activeTab === t.key;
          return (
            <button
              key={t.key}
              onClick={() => onTabChange(t.key)}
              className={`flex items-center gap-2 px-5 py-3.5 text-sm cursor-pointer transition-colors border-b-2 ${
                active
                  ? 'text-brand-400 border-brand-500 font-medium'
                  : 'text-text-secondary border-transparent hover:text-text-primary'
              }`}
            >
              <Icon size={16} />
              {t.label}
            </button>
          );
        })}
      </div>
    </nav>
  );
}
