import { useState, useEffect } from 'react';
import { Globe } from 'lucide-react';
import { useOtaConfig, useSetOtaConfig, useRunOta } from '../../hooks/useOtaConfig';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';

export function OtaSection() {
  const { data } = useOtaConfig();
  const save = useSetOtaConfig();
  const run  = useRunOta();
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
      title="OTA Updates"
      icon={<Globe size={16} />}
      description="Firmware update URL. The device downloads and flashes firmware.bin from this location."
    >
      <InputField label="Firmware URL" value={url} onChange={(e) => setUrl(e.target.value)} />
      <div className="flex flex-col gap-2">
        <label className="flex items-center gap-2 text-sm text-text-secondary cursor-pointer">
          <input type="checkbox" checked={useSsl} onChange={(e) => setUseSsl(e.target.checked)} className="rounded border-surface-border accent-brand-500" />
          Use SSL
        </label>
        <label className="flex items-center gap-2 text-sm text-text-secondary cursor-pointer">
          <input type="checkbox" checked={verifySsl} onChange={(e) => setVerifySsl(e.target.checked)} className="rounded border-surface-border accent-brand-500" />
          Verify SSL certificate
        </label>
      </div>
      <div className="flex gap-2">
        <Button
          onClick={() => save.mutate({ url, use_ssl: useSsl, verify_ssl: verifySsl })}
          disabled={save.isPending}
        >
          {save.isPending ? 'Saving…' : 'Save'}
        </Button>
        <Button variant="secondary" onClick={() => run.mutate()} disabled={run.isPending}>
          {run.isPending ? 'Starting…' : 'Update now'}
        </Button>
      </div>
    </SectionCard>
  );
}
