import { useState } from 'react';
import { Key } from 'lucide-react';
import { useSecurityKey, useSetSecurityKey } from '../../hooks/useSecurityKey';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { PasswordField } from '@spoolhard/ui/components/PasswordField';
import { Button } from '@spoolhard/ui/components/Button';

export function SecurityKeySection() {
  const { data } = useSecurityKey();
  const mutation = useSetSecurityKey();
  const [key, setKey] = useState('');

  return (
    <SectionCard
      title="Security Key"
      icon={<Key size={16} />}
      description="Shared secret for encrypted console ↔ scale messages. Must match the key set on the paired scale."
    >
      <div className="text-xs text-text-muted">
        Current: <span className="font-mono">{data?.key_preview ?? '—'}</span>
      </div>
      <PasswordField label="New key" value={key} onChange={(e) => setKey(e.target.value)} />
      <Button onClick={() => mutation.mutate({ key })} disabled={mutation.isPending || !key}>
        {mutation.isPending ? 'Saving…' : 'Save'}
      </Button>
    </SectionCard>
  );
}
