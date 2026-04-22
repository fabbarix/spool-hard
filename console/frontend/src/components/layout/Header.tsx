import { Wifi, WifiOff } from 'lucide-react';
import { useDeviceName } from '../../hooks/useDeviceName';
import { useFirmwareInfo } from '../../hooks/useFirmwareInfo';
import { useWifiStatus } from '../../hooks/useWifiStatus';
import { useScaleLink } from '../../hooks/useScaleLink';
import { useWebSocket } from '@spoolhard/ui/providers/WebSocketProvider';
import { StatusDot } from '@spoolhard/ui/components/StatusDot';

export function Header() {
  const { data: deviceName } = useDeviceName();
  const { data: fw } = useFirmwareInfo();
  const { data: wifi } = useWifiStatus();
  const { data: scale } = useScaleLink();
  const { isConnected } = useWebSocket();

  const wifiStatus: 'connected' | 'connecting' | 'disconnected' = wifi?.connected
    ? 'connected'
    : wifi?.configured ? 'connecting' : 'disconnected';

  return (
    // Outer bar spans the viewport for a continuous dark strip; the inner row
    // is constrained to the same max-w as <main> so the header contents align
    // horizontally with the page content below.
    <header className="bg-surface-header border-b border-surface-border h-14">
      <div className="mx-auto max-w-[1100px] h-full px-4 flex items-center justify-between">
        <div className="flex items-center gap-3">
          <h1 className="font-semibold text-text-primary text-base">
            {deviceName?.device_name ?? 'SpoolHard Console'}
          </h1>
          {fw && <span className="text-xs text-text-muted font-mono">v{fw.fw_version}</span>}
        </div>
        <div className="flex items-center gap-4">
          <div className="flex items-center gap-2 bg-surface-card/50 rounded-full px-3 py-1">
            {wifi?.connected ? <Wifi size={16} className="text-text-secondary" /> : <WifiOff size={16} className="text-text-muted" />}
            <StatusDot status={wifiStatus} />
            {wifi?.ssid && <span className="text-xs text-text-secondary">{wifi.ssid}</span>}
          </div>
          <div className="flex items-center gap-1.5 text-xs text-text-muted">
            <span className="font-mono">Scale</span>
            <StatusDot status={scale?.connected ? 'connected' : 'disconnected'} />
          </div>
          <div className="flex items-center gap-1.5 text-xs text-text-muted">
            <span className="font-mono">WS</span>
            <StatusDot status={isConnected ? 'connected' : 'disconnected'} />
          </div>
        </div>
      </div>
    </header>
  );
}
