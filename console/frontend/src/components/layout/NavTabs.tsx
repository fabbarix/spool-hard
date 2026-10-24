import { LayoutDashboard, Settings, Package, FlaskConical, Scale } from 'lucide-react';
import type { Tab } from '../../App';

interface NavTabsProps {
  activeTab: Tab;
  onTabChange: (tab: Tab) => void;
}

type TabDef = { key: Tab; label: string; short?: string; icon: typeof Scale };
const tabs: TabDef[] = [
  { key: 'dashboard'     as const, label: 'Dashboard',     icon: LayoutDashboard },
  { key: 'spools'        as const, label: 'Spools',        icon: Package },
  { key: 'empty-weights' as const, label: 'Empty weights', short: 'Weights', icon: Scale },
  { key: 'filaments'     as const, label: 'Filaments',     icon: FlaskConical },
  { key: 'config'        as const, label: 'Config',        icon: Settings },
];

export function NavTabs({ activeTab, onTabChange }: NavTabsProps) {
  return (
    // Same trick as <Header>: full-width strip, inner row capped at the
    // content max-width so the tabs line up with the cards below.
    // Hidden below sm: — phones get <BottomTabBar> instead (5 top tabs
    // measure ~600px and overflow a 390px viewport).
    <nav className="hidden sm:block bg-surface-header border-b border-surface-border">
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

// Phone navigation: fixed-height bar at the bottom of the app shell
// (a shrink-0 flex child, so <main> keeps scrolling above it). Five equal
// columns, icon over short label, >=48px tall touch targets.
export function BottomTabBar({ activeTab, onTabChange }: NavTabsProps) {
  return (
    <nav
      aria-label="Primary"
      className="sm:hidden shrink-0 grid grid-cols-5 bg-surface-header border-t border-surface-border pb-[env(safe-area-inset-bottom)]"
    >
      {tabs.map((t) => {
        const Icon = t.icon;
        const active = activeTab === t.key;
        return (
          <button
            key={t.key}
            onClick={() => onTabChange(t.key)}
            aria-current={active ? 'page' : undefined}
            className={`flex flex-col items-center justify-center gap-0.5 min-h-12 py-1.5 text-[11px] cursor-pointer transition-colors border-t-2 ${
              active
                ? 'text-brand-400 border-brand-500 font-medium'
                : 'text-text-secondary border-transparent'
            }`}
          >
            <Icon size={20} />
            <span className="truncate max-w-full px-0.5">{t.short ?? t.label}</span>
          </button>
        );
      })}
    </nav>
  );
}
