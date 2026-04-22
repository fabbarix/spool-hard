import { useState } from 'react';
import { RotateCcw, Trash2, Power, AlertTriangle } from 'lucide-react';
import { useRestart, useFactoryReset } from '../../hooks/useDeviceControl';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { useReconnect } from '@spoolhard/ui/providers/ReconnectProvider';

type ConfirmTarget = null | 'restart' | 'reset';

export function DeviceControlSection() {
  const restart = useRestart();
  const factoryReset = useFactoryReset();
  const reconnect = useReconnect();
  const [confirming, setConfirming] = useState<ConfirmTarget>(null);
  const [done, setDone] = useState<ConfirmTarget>(null);

  const execute = (target: ConfirmTarget) => {
    if (target === 'restart') {
      restart.mutate(undefined, {
        onSuccess: () => {
          setDone('restart');
          setConfirming(null);
          reconnect.start('Restarting device…');
        },
      });
    } else if (target === 'reset') {
      factoryReset.mutate(undefined, {
        onSuccess: () => {
          setDone('reset');
          setConfirming(null);
          reconnect.start('Resetting device…');
        },
      });
    }
  };

  const isPending = restart.isPending || factoryReset.isPending;

  if (done) {
    return (
      <SectionCard title="Device Control" icon={<Power size={16} />}>
        <div className="flex items-center gap-2 text-sm text-status-connected">
          <RotateCcw size={14} className="animate-spin" />
          {done === 'restart' ? 'Restarting device...' : 'Resetting device...'}
        </div>
        <div className="text-xs text-text-muted">
          The page will reload when the device is back online.
        </div>
      </SectionCard>
    );
  }

  if (confirming) {
    const isReset = confirming === 'reset';
    return (
      <SectionCard title="Device Control" icon={<Power size={16} />}>
        <div className="flex items-center gap-2 rounded-card border border-status-error/30 bg-status-error/5 p-4">
          <AlertTriangle size={20} className="text-status-error shrink-0" />
          <div className="flex-1">
            <div className="text-sm font-medium text-text-primary">
              {isReset ? 'Factory Reset' : 'Restart Device'}
            </div>
            <div className="text-xs text-text-secondary mt-0.5">
              {isReset
                ? 'This will erase all settings (WiFi, security key, calibration) and reboot. This cannot be undone.'
                : 'The device will reboot. Unsaved changes will be lost.'}
            </div>
          </div>
        </div>
        <div className="flex gap-2">
          <Button
            variant={isReset ? 'danger' : 'primary'}
            onClick={() => execute(confirming)}
            disabled={isPending}
          >
            {isPending
              ? (isReset ? 'Resetting...' : 'Restarting...')
              : (isReset ? 'Yes, erase everything' : 'Yes, restart')}
          </Button>
          <Button variant="secondary" onClick={() => setConfirming(null)} disabled={isPending}>
            Cancel
          </Button>
        </div>
      </SectionCard>
    );
  }

  return (
    <SectionCard title="Device Control" icon={<Power size={16} />}>
      <div className="flex gap-3">
        <Button variant="secondary" onClick={() => setConfirming('restart')}>
          <RotateCcw size={14} className="mr-1.5 inline" />
          Restart
        </Button>
        <Button variant="danger" onClick={() => setConfirming('reset')}>
          <Trash2 size={14} className="mr-1.5 inline" />
          Factory Reset
        </Button>
      </div>
    </SectionCard>
  );
}
