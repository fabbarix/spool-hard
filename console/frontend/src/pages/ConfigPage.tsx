import { useState, useEffect } from 'react';
import { Cpu, Scale, Printer, Shield, Wrench, Bug } from 'lucide-react';
import { SubTabBar, type SubTab } from '@spoolhard/ui/components/SubTabBar';
import { WifiSection } from '../components/config/WifiSection';
import { DeviceNameSection } from '../components/config/DeviceNameSection';
import { OtaSection } from '../components/config/OtaSection';
import { SecurityKeySection } from '../components/config/SecurityKeySection';
import { ScaleSection } from '../components/config/ScaleSection';
import { PrintersSection } from '../components/config/PrintersSection';
import { DirectUploadSection } from '../components/config/DirectUploadSection';
import { DeviceControlSection } from '../components/config/DeviceControlSection';
import { DisplaySection } from '../components/config/DisplaySection';
import { QuickWeightsSection } from '../components/config/QuickWeightsSection';
import { FilamentsDbSection } from '../components/config/FilamentsDbSection';
import { DebugSection } from '../components/config/DebugSection';

type ConfigTab = 'setup' | 'scale' | 'printers' | 'security' | 'device' | 'debug';

const tabs: SubTab<ConfigTab>[] = [
  { id: 'setup',    label: 'Setup',    icon: <Cpu size={14} /> },
  { id: 'scale',    label: 'Scale',    icon: <Scale size={14} /> },
  { id: 'printers', label: 'Printers', icon: <Printer size={14} /> },
  { id: 'security', label: 'Security', icon: <Shield size={14} /> },
  { id: 'device',   label: 'Device',   icon: <Wrench size={14} /> },
  { id: 'debug',    label: 'Debug',    icon: <Bug size={14} /> },
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
      <SubTabBar tabs={tabs} active={activeTab} onChange={navigate} />

      <div className="animate-in">
        {activeTab === 'setup' && (
          <div className="space-y-4">
            <DeviceNameSection />
            <WifiSection />
            <DisplaySection />
          </div>
        )}

        {activeTab === 'scale' && (
          <div className="space-y-4">
            <ScaleSection />
            <QuickWeightsSection />
          </div>
        )}

        {activeTab === 'printers' && <PrintersSection />}

        {activeTab === 'security' && <SecurityKeySection />}

        {activeTab === 'device' && (
          <div className="space-y-4">
            <OtaSection />
            <DirectUploadSection />
            <FilamentsDbSection />
            <DeviceControlSection />
          </div>
        )}

        {activeTab === 'debug' && <DebugSection />}
      </div>
    </div>
  );
}
