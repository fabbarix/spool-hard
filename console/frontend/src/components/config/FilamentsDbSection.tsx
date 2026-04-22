import { Database, Trash2 } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { Button } from '@spoolhard/ui/components/Button';
import { DropZone, type FileValidation } from '@spoolhard/ui/components/DropZone';
import {
  useFilamentsDbInfo,
  useFilamentsDbUpload,
  useFilamentsDbDelete,
} from '../../hooks/useFilamentsDb';

// SQLite file header magic — a well-formed file starts with exactly these
// 16 bytes. Cheap client-side guard against uploading a random .bin.
const SQLITE_MAGIC = 'SQLite format 3\0';

async function validateFilamentsDb(file: File): Promise<FileValidation> {
  if (file.size < 16) return { ok: false, error: 'File too small to be a SQLite database' };
  if (file.size > 16 * 1024 * 1024) {
    return { ok: false, error: `File (${(file.size / 1024 / 1024).toFixed(1)} MB) exceeds 16 MB cap` };
  }
  const head = new Uint8Array(await file.slice(0, 16).arrayBuffer());
  const headStr = new TextDecoder().decode(head);
  if (headStr !== SQLITE_MAGIC) {
    return { ok: false, error: 'Not a SQLite database (missing "SQLite format 3" magic).' };
  }
  return { ok: true, info: `SQLite database · ${(file.size / 1024).toFixed(0)} KB` };
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
      description="Upload a bambu-filaments SQLite database to the SD card. The spool detail page will use it to pre-fill brand / material / temps / filament ID when you click 'Load from library'."
    >
      <div className="grid grid-cols-1 md:grid-cols-[1fr_auto] gap-4 items-start">
        <DropZone
          label="filaments.db"
          icon={<Database size={24} strokeWidth={1.5} />}
          validate={validateFilamentsDb}
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
        Generate <span className="font-mono">filaments.db</span> from the
        <a href="https://github.com/" className="underline ml-1">bambu-filaments</a> tool:
        <span className="block font-mono mt-1">uv run python main.py</span>
      </div>
    </SectionCard>
  );
}
