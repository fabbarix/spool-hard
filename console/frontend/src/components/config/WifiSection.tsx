import { useEffect, useMemo, useState } from 'react';
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
  const [pinEnabled, setPinEnabled] = useState(false);
  const [pinnedBssid, setPinnedBssid] = useState('');

  // Hydrate pin state from current snapshot once it arrives.
  useEffect(() => {
    if (wifi?.pinned_bssid) {
      setPinEnabled(true);
      setPinnedBssid(wifi.pinned_bssid);
    }
  }, [wifi?.pinned_bssid]);

  const wifiStatus: 'connected' | 'connecting' | 'disconnected' = wifi?.connected
    ? 'connected'
    : wifi?.configured ? 'connecting' : 'disconnected';

  const matchingBssids = useMemo(() => {
    const list = networks ?? [];
    const targetSsid = ssid || wifi?.ssid || '';
    return [...list]
      .filter((n) => n.ssid === targetSsid && n.bssid)
      .sort((a, b) => b.rssi - a.rssi);
  }, [networks, ssid, wifi?.ssid]);

  const distinctSsids = useMemo(() => {
    const seen = new Set<string>();
    const out: { ssid: string; rssi: number; secure: boolean }[] = [];
    for (const n of networks ?? []) {
      if (seen.has(n.ssid)) continue;
      seen.add(n.ssid);
      out.push({ ssid: n.ssid, rssi: n.rssi, secure: n.secure });
    }
    return out.sort((a, b) => b.rssi - a.rssi);
  }, [networks]);

  return (
    <SectionCard title="WiFi" icon={<Wifi size={16} />}>
      <div className="flex items-center gap-2 text-sm">
        <StatusDot status={wifiStatus} />
        <span className="text-text-secondary">
          {wifi?.connected
            ? `Connected to ${wifi.ssid} (${wifi.ip}, ${wifi.rssi} dBm)`
            : wifi?.configured ? 'Connecting…' : 'Not configured'}
        </span>
      </div>
      {wifi?.connected && wifi.bssid && (
        <div className="text-xs text-text-secondary pl-5">
          AP {wifi.bssid} · channel {wifi.channel}
          {wifi.pinned_bssid &&
            (wifi.pinned_bssid.toLowerCase() === wifi.bssid.toLowerCase()
              ? ' · pinned'
              : ` · pin set to ${wifi.pinned_bssid} (fallback active)`)}
        </div>
      )}

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
              {distinctSsids.map((n) => (
                <option key={n.ssid} value={n.ssid}>{n.ssid} ({n.rssi} dBm){n.secure ? ' *' : ''}</option>
              ))}
            </select>
          </label>
        </div>
        <Button variant="secondary" onClick={() => refetch()} disabled={isFetching}>
          {isFetching ? 'Scanning…' : 'Scan'}
        </Button>
      </div>

      <InputField label="SSID" value={ssid} onChange={(e) => setSsid(e.target.value)} />
      <PasswordField label="Password" value={password} onChange={(e) => setPassword(e.target.value)} />

      <label className="flex items-center gap-2 text-sm cursor-pointer">
        <input
          type="checkbox"
          checked={pinEnabled}
          onChange={(e) => {
            setPinEnabled(e.target.checked);
            if (!e.target.checked) setPinnedBssid('');
          }}
        />
        <span className="text-text-secondary">
          Pin to specific access point (mesh node)
        </span>
      </label>
      {pinEnabled && (
        <label className="block text-sm">
          <span className="mb-1 block text-sm text-text-secondary">
            BSSID — pick the same node on each device
          </span>
          <select
            className="w-full bg-surface-input border border-surface-border rounded-button px-3 py-2 text-sm text-text-primary focus:outline-none focus:border-brand-500/50 focus:ring-1 focus:ring-brand-500/20 transition-colors"
            value={pinnedBssid}
            onChange={(e) => setPinnedBssid(e.target.value)}
          >
            <option value="">-- Select BSSID --</option>
            {matchingBssids.map((n) => (
              <option key={n.bssid} value={n.bssid}>
                {n.bssid} (ch {n.channel}, {n.rssi} dBm)
              </option>
            ))}
          </select>
          <p className="mt-1 text-xs text-text-secondary">
            If the pinned node is unreachable for 60 s, the device falls back to
            auto-select for this session. The pin is preserved in NVS.
          </p>
        </label>
      )}

      <Button
        onClick={() => {
          const body: { ssid: string; pass: string; pinned_bssid?: string } = {
            ssid,
            pass: password,
          };
          if (pinEnabled || wifi?.pinned_bssid) {
            body.pinned_bssid = pinEnabled ? pinnedBssid : '';
          }
          saveMutation.mutate(body);
        }}
        disabled={saveMutation.isPending || !ssid || (pinEnabled && !pinnedBssid)}
      >
        {saveMutation.isPending ? 'Connecting…' : 'Save & Connect'}
      </Button>
    </SectionCard>
  );
}
