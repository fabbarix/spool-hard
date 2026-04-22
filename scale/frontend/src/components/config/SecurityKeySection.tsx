import { useState } from 'react';
import { Shield } from 'lucide-react';
import { useSecurityKey, useSetSecurityKey } from '../../hooks/useSecurityKey';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { PasswordField } from '@spoolhard/ui/components/PasswordField';
import { Button } from '@spoolhard/ui/components/Button';

export function SecurityKeySection() {
  const { data } = useSecurityKey();
  const mutation = useSetSecurityKey();
  const [key, setKey] = useState('');

  return (
    <SectionCard title="Security Key" icon={<Shield size={16} />}>
      <div className="text-sm text-text-secondary">
        {data?.configured
          ? <span>Configured (preview: <span className="font-mono text-text-data">{data.key_preview}</span>)</span>
          : 'Not configured'}
      </div>
      <PasswordField label="New key" value={key} onChange={(e) => setKey(e.target.value)} />
      <Button
        onClick={() => mutation.mutate({ key })}
        disabled={mutation.isPending || !key}
      >
        {mutation.isPending ? 'Saving...' : 'Save'}
      </Button>
    </SectionCard>
  );
}
