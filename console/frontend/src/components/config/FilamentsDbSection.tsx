import { Database, Trash2 } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { DropZone, type FileValidation } from '@spoolhard/ui/components/DropZone';
import {
  useFilamentsDbInfo,
  useFilamentsDbUpload,
  useFilamentsDbDelete,
} from '../../hooks/useFilamentsDb';

// Cheap client-side guard against uploading a random .bin: the file must
// be valid JSONL with at least one line that parses as an object carrying
// the `setting_id` key. We only inspect the first ~1KB so the check stays
// fast even if the user picks a multi-MB file.
async function validateFilamentsJsonl(file: File): Promise<FileValidation> {
  if (file.size < 8) return { ok: false, error: 'File too small to be filaments.jsonl' };
  if (file.size > 16 * 1024 * 1024) {
    return { ok: false, error: `File (${(file.size / 1024 / 1024).toFixed(1)} MB) exceeds 16 MB cap` };
  }
  const head = new TextDecoder().decode(
    new Uint8Array(await file.slice(0, 1024).arrayBuffer()),
  );
  const firstLine = head.split('\n')[0]?.trim();
  if (!firstLine) return { ok: false, error: 'File is empty.' };
  try {
    const obj = JSON.parse(firstLine);
    if (typeof obj !== 'object' || obj === null || !obj.setting_id) {
      return { ok: false, error: 'First row missing setting_id — wrong file format.' };
    }
  } catch {
    return { ok: false, error: 'First line isn\'t valid JSON — not a filaments.jsonl file.' };
  }
  return { ok: true, info: `Filaments JSONL · ${(file.size / 1024).toFixed(0)} KB` };
}

function formatAge(mtime_s?: number): string {
  if (!mtime_s) return 'unknown';
  const ageS = Date.now() / 1000 - mtime_s;
  if (ageS < 60) return `${Math.floor(ageS)}s ago`;
  if (ageS < 3600) return `${Math.floor(ageS / 60)}m ago`;
  if (ageS < 86400) return `${Math.floor(ageS / 3600)}h ago`;
  return `${Math.floor(ageS / 86400)}d ago`;
}

export function FilamentsDbSection() {
  const { data: info } = useFilamentsDbInfo();
  const upload = useFilamentsDbUpload();
  const del    = useFilamentsDbDelete();

  return (
    <SectionCard
      title="Filaments Library"
      icon={<Database size={16} />}
      description="Upload a flat filaments.jsonl (built from the BambuStudio profiles) to the SD card. The spool detail page + LCD spool wizard use it to pre-fill brand / material / temps / filament ID when you click 'Load from library'."
    >
      <div className="grid grid-cols-1 md:grid-cols-[1fr_auto] gap-4 items-start">
        <DropZone
          label="filaments.jsonl"
          icon={<Database size={24} strokeWidth={1.5} />}
          validate={validateFilamentsJsonl}
          onUpload={(file) => upload.mutate(file)}
          isPending={upload.isPending}
          isSuccess={upload.isSuccess}
          progress={upload.progress}
          error={upload.error instanceof Error ? upload.error.message : undefined}
        />

        <div className="space-y-2 min-w-[160px]">
          {info?.present ? (
            <div className="rounded-md border border-surface-border bg-surface-input p-3 text-xs space-y-1 font-mono">
              <div className="text-text-primary">
                {info.size != null ? `${(info.size / 1024).toFixed(0)} KB` : '—'}
              </div>
              <div className="text-text-muted">updated {formatAge(info.mtime_s)}</div>
              <Button
                variant="secondary"
                onClick={() => {
                  if (confirm('Remove the uploaded filaments library? You can upload a new one afterward.')) {
                    del.mutate();
                  }
                }}
                disabled={del.isPending}
                className="w-full mt-2"
              >
                <Trash2 size={12} className="mr-1.5 inline" />
                Remove
              </Button>
            </div>
          ) : (
            <div className="rounded-md border border-dashed border-surface-border p-3 text-xs text-text-muted italic">
              No library uploaded yet.
            </div>
          )}
          {del.error instanceof Error && (
            <div className="text-xs text-status-error">{del.error.message}</div>
          )}
        </div>
      </div>

      <div className="pt-2 text-[11px] text-text-muted leading-relaxed">
        Build <span className="font-mono">filaments.jsonl</span> from the
        upstream BambuStudio profiles:
        <span className="block font-mono mt-1">./scripts/build_filaments_db.sh</span>
        Each release also bundles a fresh copy as
        {' '}<span className="font-mono">spoolhard-console-filaments.jsonl</span>.
      </div>
    </SectionCard>
  );
}
