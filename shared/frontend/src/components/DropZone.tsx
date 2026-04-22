import { useState, useRef, useEffect, useCallback, type ReactNode } from 'react';
import { Upload, X, CheckCircle, AlertTriangle } from 'lucide-react';

export interface FileValidation {
  ok: boolean;
  error?: string;
  info?: string;
}

interface DropZoneProps {
  label: string;
  accept?: string;
  icon?: ReactNode;
  onUpload: (file: File) => void;
  validate?: (file: File) => Promise<FileValidation>;
  disabled?: boolean;
  progress?: number;
  isPending?: boolean;
  isSuccess?: boolean;
  /**
   * Server-side error message surfaced after the upload response is received.
   * Typically passed through from a TanStack mutation (`mutation.error?.message`)
   * so the user sees e.g. "upload rejected (wrong product) — ..." from the
   * firmware's 400 body, not just a silent reset to the staged-file state.
   */
  error?: string;
}

export function DropZone({ label, accept = '.bin', icon, onUpload, validate, disabled, progress = 0, isPending, isSuccess, error }: DropZoneProps) {
  const [dragOver, setDragOver] = useState(false);
  const [stagedFile, setStagedFile] = useState<File | null>(null);
  const [validation, setValidation] = useState<FileValidation | null>(null);
  const [validating, setValidating] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);
  const zoneRef = useRef<HTMLDivElement>(null);
  const dragCounter = useRef(0);

  const stageFile = useCallback(async (file: File) => {
    setStagedFile(file);
    setValidation(null);
    if (validate) {
      setValidating(true);
      try {
        const result = await validate(file);
        setValidation(result);
      } catch {
        setValidation({ ok: false, error: 'Validation failed' });
      }
      setValidating(false);
    } else {
      setValidation({ ok: true });
    }
  }, [validate]);

  // Native DOM drag events — bypasses React's event delegation for reliability.
  useEffect(() => {
    const zone = zoneRef.current;
    if (!zone) return;

    const onDragEnter = (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      dragCounter.current++;
      if (!disabled && !isPending) setDragOver(true);
    };

    const onDragOver = (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
    };

    const onDragLeave = (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      dragCounter.current--;
      if (dragCounter.current <= 0) {
        dragCounter.current = 0;
        setDragOver(false);
      }
    };

    const onDrop = (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      dragCounter.current = 0;
      setDragOver(false);
      if (disabled || isPending) return;
      const file = e.dataTransfer?.files[0];
      if (file) stageFile(file);
    };

    zone.addEventListener('dragenter', onDragEnter);
    zone.addEventListener('dragover', onDragOver);
    zone.addEventListener('dragleave', onDragLeave);
    zone.addEventListener('drop', onDrop);

    return () => {
      zone.removeEventListener('dragenter', onDragEnter);
      zone.removeEventListener('dragover', onDragOver);
      zone.removeEventListener('dragleave', onDragLeave);
      zone.removeEventListener('drop', onDrop);
    };
  }, [disabled, isPending, stageFile]);

  const handleSelect = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (file) stageFile(file);
  };

  const handleUpload = () => {
    if (stagedFile && !isPending && validation?.ok) {
      onUpload(stagedFile);
    }
  };

  const clearStaged = () => {
    setStagedFile(null);
    setValidation(null);
    if (inputRef.current) inputRef.current.value = '';
  };

  const canFlash = stagedFile && validation?.ok && !isPending && !validating;
  // Server-side rejection (400/500) comes through after isPending flips back
  // to false. We want the red error state to stay on the zone until the user
  // dismisses the file, so treat `error` like a validation failure visually.
  const showServerError = !!error && !isPending && !isSuccess;

  return (
    <div className="space-y-2">
      <div
        ref={zoneRef}
        onClick={(!disabled && !isPending && !stagedFile) ? () => inputRef.current?.click() : undefined}
        className={`
          relative flex flex-col items-center justify-center gap-2
          rounded-card border-2 border-dashed p-6
          transition-all duration-200
          ${!stagedFile && !isPending ? 'cursor-pointer' : ''}
          ${dragOver
            ? 'border-brand-400 bg-brand-500/10 shadow-[0_0_20px_rgba(240,180,41,0.08)]'
            : (validation && !validation.ok) || showServerError
              ? 'border-status-error/40 bg-status-error/5'
              : stagedFile && !isPending
                ? 'border-brand-500/40 bg-brand-500/5'
                : 'border-surface-border hover:border-text-muted hover:bg-surface-card-hover'}
          ${disabled ? 'opacity-50 cursor-not-allowed' : ''}
        `}
      >
        <input
          ref={inputRef}
          type="file"
          accept={accept}
          onChange={handleSelect}
          className="hidden"
          disabled={disabled || isPending}
        />

        {isPending ? (
          <div className="flex flex-col items-center gap-2 w-full">
            <div className="text-sm font-medium text-brand-400">
              Uploading {stagedFile?.name}... {progress}%
            </div>
            <div className="w-full max-w-56 h-1.5 overflow-hidden rounded-full bg-surface-border">
              <div
                className="h-full bg-brand-500 rounded-full transition-all duration-300"
                style={{ width: `${progress}%` }}
              />
            </div>
            <div className="text-[11px] text-text-muted">Device will reboot when complete</div>
          </div>
        ) : isSuccess ? (
          <div className="flex flex-col items-center gap-1">
            <CheckCircle size={24} className="text-status-connected" />
            <div className="text-sm font-medium text-status-connected">Upload complete — rebooting</div>
          </div>
        ) : stagedFile ? (
          <div className="flex flex-col items-center gap-2 w-full">
            <div className={(validation && !validation.ok) || showServerError ? 'text-status-error' : 'text-text-muted'}>
              {(validation && !validation.ok) || showServerError
                ? <AlertTriangle size={24} strokeWidth={1.5} />
                : icon || <Upload size={24} strokeWidth={1.5} />}
            </div>
            <div className="flex items-center gap-2">
              <span className={`text-sm font-medium font-mono ${(validation && !validation.ok) || showServerError ? 'text-status-error' : 'text-brand-400'}`}>
                {stagedFile.name}
              </span>
              <button
                onClick={(e) => { e.stopPropagation(); clearStaged(); }}
                className="text-text-muted hover:text-status-error transition-colors p-0.5"
              >
                <X size={14} />
              </button>
            </div>

            {validating ? (
              <div className="text-[11px] text-text-muted">Checking file...</div>
            ) : showServerError ? (
              <div className="text-[11px] text-status-error text-center max-w-64">
                {error}
              </div>
            ) : validation && !validation.ok ? (
              <div className="text-[11px] text-status-error text-center max-w-64">
                {validation.error}
              </div>
            ) : validation?.info ? (
              <div className="text-[11px] text-text-muted text-center font-mono">
                {validation.info}
              </div>
            ) : (
              <div className="text-[11px] text-text-muted">
                {(stagedFile.size / 1024).toFixed(0)} KB
              </div>
            )}

            {canFlash && (
              <button
                onClick={(e) => { e.stopPropagation(); handleUpload(); }}
                className="mt-1 flex items-center gap-1.5 bg-brand-500 hover:bg-brand-400 text-surface-body font-semibold text-sm px-4 py-2 rounded-button transition-colors"
              >
                <Upload size={14} />
                Flash to device
              </button>
            )}
          </div>
        ) : (
          <>
            <div className="text-text-muted">
              {icon || <Upload size={24} strokeWidth={1.5} />}
            </div>
            <div className="text-sm font-medium text-text-secondary">{label}</div>
            <div className="text-xs text-text-muted">
              Drop a <span className="font-mono text-text-secondary">.bin</span> file here or click to browse
            </div>
          </>
        )}
      </div>
    </div>
  );
}
