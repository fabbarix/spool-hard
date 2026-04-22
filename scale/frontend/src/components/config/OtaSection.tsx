import { useState, useEffect } from 'react';
import { Globe } from 'lucide-react';
import { useOtaConfig, useSetOtaConfig } from '../../hooks/useOtaConfig';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';

export function OtaSection() {
  const { data } = useOtaConfig();
  const mutation = useSetOtaConfig();
  const [url, setUrl] = useState('');
  const [useSsl, setUseSsl] = useState(false);
  const [verifySsl, setVerifySsl] = useState(false);

  useEffect(() => {
    if (data) {
      setUrl(data.url);
      setUseSsl(data.use_ssl);
      setVerifySsl(data.verify_ssl);
    }
  }, [data]);

  return (
    <SectionCard
      title="OTA Server"
      icon={<Globe size={16} />}
      description="Configure the remote server for over-the-air updates. The device will check for a manifest.json at this URL."
    >
      <InputField label="Manifest URL" value={url} onChange={(e) => setUrl(e.target.value)} />
      <div className="flex flex-col gap-2">
        <label className="flex items-center gap-2 text-sm text-text-secondary cursor-pointer">
          <input
            type="checkbox"
            checked={useSsl}
            onChange={(e) => setUseSsl(e.target.checked)}
            className="rounded border-surface-border accent-brand-500"
          />
          Use SSL
        </label>
        <label className="flex items-center gap-2 text-sm text-text-secondary cursor-pointer">
          <input
            type="checkbox"
            checked={verifySsl}
            onChange={(e) => setVerifySsl(e.target.checked)}
            className="rounded border-surface-border accent-brand-500"
          />
          Verify SSL certificate
        </label>
      </div>
      <Button
        onClick={() => mutation.mutate({ url, use_ssl: useSsl, verify_ssl: verifySsl })}
        disabled={mutation.isPending}
      >
        {mutation.isPending ? 'Saving...' : 'Save'}
      </Button>
    </SectionCard>
  );
}
