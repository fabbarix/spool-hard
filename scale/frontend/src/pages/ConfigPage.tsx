import { useState, useEffect } from 'react';
import { Cpu, Scale, Shield, Wrench } from 'lucide-react';
import { DeviceNameSection } from '../components/config/DeviceNameSection';
import { WifiSection } from '../components/config/WifiSection';
import { SecurityKeySection } from '../components/config/SecurityKeySection';
import { ScaleSection } from '../components/config/ScaleSection';
import { OtaSection } from '../components/config/OtaSection';
import { DirectUploadSection } from '../components/config/DirectUploadSection';
import { DeviceControlSection } from '../components/config/DeviceControlSection';

type ConfigTab = 'setup' | 'scale' | 'security' | 'device';

const tabs: { id: ConfigTab; label: string; icon: React.ReactNode }[] = [
  { id: 'setup',    label: 'Setup',    icon: <Cpu size={14} /> },
  { id: 'scale',    label: 'Scale',    icon: <Scale size={14} /> },
  { id: 'security', label: 'Security', icon: <Shield size={14} /> },
  { id: 'device',   label: 'Device',   icon: <Wrench size={14} /> },
];

function getInitialTab(): ConfigTab {
  const params = new URLSearchParams(window.location.search);
  const t = params.get('tab');
  if (t && tabs.some((tab) => tab.id === t)) return t as ConfigTab;
  return 'setup';
}

export function ConfigPage() {
  const [activeTab, setActiveTab] = useState<ConfigTab>(getInitialTab);

  const navigate = (t: ConfigTab) => {
    const url = new URL(window.location.href);
    url.searchParams.set('tab', t);
    window.history.replaceState(null, '', url.toString());
    setActiveTab(t);
  };

  useEffect(() => {
    const onPop = () => setActiveTab(getInitialTab());
    window.addEventListener('popstate', onPop);
    return () => window.removeEventListener('popstate', onPop);
  }, []);

  return (
    <div className="space-y-4">
      {/* Sub-tab bar */}
      <div className="flex gap-1 rounded-card bg-surface-card border border-surface-border p-1">
        {tabs.map((tab) => (
          <button
            key={tab.id}
            onClick={() => navigate(tab.id)}
            className={`
              flex items-center gap-1.5 px-4 py-2 rounded-[7px] text-sm transition-all duration-200
              ${activeTab === tab.id
                ? 'bg-brand-500/15 text-brand-400 font-medium shadow-sm'
                : 'text-text-secondary hover:text-text-primary hover:bg-surface-card-hover'}
            `}
          >
            {tab.icon}
            {tab.label}
          </button>
        ))}
      </div>

      {/* Tab content */}
      <div className="animate-in">
        {activeTab === 'setup' && (
          <div className="space-y-4">
            <DeviceNameSection />
            <WifiSection />
          </div>
        )}

        {activeTab === 'scale' && <ScaleSection />}

        {activeTab === 'security' && <SecurityKeySection />}

        {activeTab === 'device' && (
          <div className="space-y-4">
            <OtaSection />
            <DirectUploadSection />
            <DeviceControlSection />
          </div>
        )}
      </div>
    </div>
  );
}
