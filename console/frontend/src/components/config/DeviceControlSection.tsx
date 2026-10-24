import { useState } from 'react';
import { RotateCcw, Trash2, Power } from 'lucide-react';
import { useRestart, useFactoryReset } from '../../hooks/useDeviceControl';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { ConfirmDialog } from '@spoolhard/ui/components/ConfirmDialog';
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
      <ConfirmDialog
        open={confirming === 'restart'}
        title="Restart Device"
        message="The device will reboot. Unsaved changes will be lost."
        variant="primary"
        confirmLabel={restart.isPending ? 'Restarting...' : 'Yes, restart'}
        busy={isPending}
        onConfirm={() => execute('restart')}
        onCancel={() => setConfirming(null)}
      />
      <ConfirmDialog
        open={confirming === 'reset'}
        title="Factory Reset"
        message="This will erase WiFi, security key, paired scale, printers, and the spool database, then reboot. This cannot be undone."
        requireText="RESET"
        confirmLabel={factoryReset.isPending ? 'Resetting...' : 'Erase everything'}
        busy={isPending}
        onConfirm={() => execute('reset')}
        onCancel={() => setConfirming(null)}
      />
    </SectionCard>
  );
}
