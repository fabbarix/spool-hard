import { useState } from 'react';
import { Wifi } from 'lucide-react';
import { useWifiStatus } from '../../hooks/useWifiStatus';
import { useWifiScan } from '../../hooks/useWifiScan';
import { useSaveWifi } from '../../hooks/useSaveWifi';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { PasswordField } from '@spoolhard/ui/components/PasswordField';
import { Button } from '@spoolhard/ui/components/Button';
import { StatusDot } from '@spoolhard/ui/components/StatusDot';

export function WifiSection() {
  const { data: wifi } = useWifiStatus();
  const { data: networks, refetch, isFetching } = useWifiScan();
  const saveMutation = useSaveWifi();
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');

  const wifiStatus: 'connected' | 'connecting' | 'disconnected' = wifi?.connected
    ? 'connected'
    : wifi?.configured
      ? 'connecting'
      : 'disconnected';

  return (
    <SectionCard title="WiFi" icon={<Wifi size={16} />}>
      <div className="flex items-center gap-2 text-sm">
        <StatusDot status={wifiStatus} />
        <span className="text-text-secondary">
          {wifi?.connected
            ? `Connected to ${wifi.ssid} (${wifi.ip}, ${wifi.rssi} dBm)`
            : wifi?.configured
              ? 'Connecting...'
              : 'Not configured'}
        </span>
      </div>

      <div className="flex items-end gap-2">
        <div className="flex-1">
          <label className="block text-sm">
            <span className="mb-1 block text-sm text-text-secondary">Network</span>
            <select
              className="w-full bg-surface-input border border-surface-border rounded-button px-3 py-2 text-sm text-text-primary focus:outline-none focus:border-brand-500/50 focus:ring-1 focus:ring-brand-500/20 transition-colors"
              value={ssid}
              onChange={(e) => setSsid(e.target.value)}
            >
              <option value="">-- Select --</option>
              {networks?.map((n) => (
                <option key={n.ssid} value={n.ssid}>
                  {n.ssid} ({n.rssi} dBm){n.secure ? ' *' : ''}
                </option>
              ))}
            </select>
          </label>
        </div>
        <Button variant="secondary" onClick={() => refetch()} disabled={isFetching}>
          {isFetching ? 'Scanning...' : 'Scan'}
        </Button>
      </div>

      <InputField label="SSID" value={ssid} onChange={(e) => setSsid(e.target.value)} />
      <PasswordField label="Password" value={password} onChange={(e) => setPassword(e.target.value)} />
      <Button
        onClick={() => saveMutation.mutate({ ssid, pass: password })}
        disabled={saveMutation.isPending || !ssid}
      >
        {saveMutation.isPending ? 'Connecting...' : 'Save & Connect'}
      </Button>
    </SectionCard>
  );
}
