import { useState, useEffect } from 'react';
import { Cpu } from 'lucide-react';
import { useDeviceName, useSetDeviceName } from '../../hooks/useDeviceName';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';

export function DeviceNameSection() {
  const { data } = useDeviceName();
  const mutation = useSetDeviceName();
  const [name, setName] = useState('');

  useEffect(() => {
    if (data?.device_name) setName(data.device_name);
  }, [data]);

  return (
    <SectionCard title="Device Name" icon={<Cpu size={16} />}>
      <InputField label="Name" value={name} onChange={(e) => setName(e.target.value)} />
      <Button
        onClick={() => mutation.mutate({ device_name: name })}
        disabled={mutation.isPending}
      >
        {mutation.isPending ? 'Saving...' : 'Save'}
      </Button>
    </SectionCard>
  );
}
