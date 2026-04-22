import { useRef } from 'react';
import { Upload } from 'lucide-react';
import { useFirmwareInfo } from '../../hooks/useFirmwareInfo';
import { useFirmwareUpload } from '../../hooks/useFirmwareUpload';
import { useSpiffsUpload } from '../../hooks/useSpiffsUpload';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';

export function FirmwareUploadSection() {
  const { data: fw } = useFirmwareInfo();
  const fwUpload = useFirmwareUpload();
  const spiffsUpload = useSpiffsUpload();
  const fwFileRef = useRef<HTMLInputElement>(null);
  const spiffsFileRef = useRef<HTMLInputElement>(null);

  return (
    <SectionCard title="Firmware Upload" icon={<Upload size={16} />}>
      {fw && (
        <div className="grid grid-cols-2 gap-2 font-mono text-xs text-text-muted">
          <div>Backend: v{fw.fw_version}</div>
          <div>Frontend: v{fw.fe_version}</div>
          <div>Flash: {(fw.flash_size / 1024).toFixed(0)} KB</div>
          <div>
            Frontend: {(fw.spiffs_used / 1024).toFixed(0)} / {(fw.spiffs_total / 1024).toFixed(0)} KB
          </div>
          <div>Free heap: {(fw.free_heap / 1024).toFixed(0)} KB</div>
        </div>
      )}

      <div className="space-y-3">
        <div>
          <label className="mb-1 block text-sm text-text-secondary">
            Backend firmware (.bin)
          </label>
          <input ref={fwFileRef} type="file" accept=".bin" className="text-sm text-text-secondary" />
          <div className="mt-2 flex items-center gap-3">
            <Button
              onClick={() => {
                const file = fwFileRef.current?.files?.[0];
                if (file) fwUpload.mutate(file);
              }}
              disabled={fwUpload.isPending}
            >
              <Upload size={14} className="mr-1 inline" />
              {fwUpload.isPending ? `Uploading ${fwUpload.progress}%` : 'Upload firmware'}
            </Button>
          </div>
          {fwUpload.isPending && (
            <div className="mt-1 h-1 overflow-hidden rounded-full bg-surface-border">
              <div
                className="h-full bg-brand-500 rounded-full transition-all"
                style={{ width: `${fwUpload.progress}%` }}
              />
            </div>
          )}
        </div>

        <div>
          <label className="mb-1 block text-sm text-text-secondary">
            Frontend (.bin)
          </label>
          <input ref={spiffsFileRef} type="file" accept=".bin" className="text-sm text-text-secondary" />
          <div className="mt-2 flex items-center gap-3">
            <Button
              onClick={() => {
                const file = spiffsFileRef.current?.files?.[0];
                if (file) spiffsUpload.mutate(file);
              }}
              disabled={spiffsUpload.isPending}
            >
              <Upload size={14} className="mr-1 inline" />
              {spiffsUpload.isPending ? `Uploading ${spiffsUpload.progress}%` : 'Upload frontend'}
            </Button>
          </div>
          {spiffsUpload.isPending && (
            <div className="mt-1 h-1 overflow-hidden rounded-full bg-surface-border">
              <div
                className="h-full bg-brand-500 rounded-full transition-all"
                style={{ width: `${spiffsUpload.progress}%` }}
              />
            </div>
          )}
        </div>
      </div>
    </SectionCard>
  );
}
