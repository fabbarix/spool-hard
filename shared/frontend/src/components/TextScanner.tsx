import { useEffect, useRef, useState } from 'react';
import { ScanText, X } from 'lucide-react';
import { Button } from './Button';

// Minimal types for the Web Shape Detection API's TextDetector (Chrome Android
// + newer Android WebViews; unavailable on iOS Safari and most desktops).
interface DetectedText {
  rawValue: string;
}
interface TextDetectorApi {
  detect(source: ImageBitmapSource): Promise<DetectedText[]>;
}
declare global {
  interface Window { TextDetector?: { new(): TextDetectorApi }; }
}

export function isTextDetectorSupported(): boolean {
  return typeof window !== 'undefined' && 'TextDetector' in window;
}

interface TextScannerProps {
  /** Button label. Kept short because this usually sits beside an input. */
  label?: string;
  /** Fired with the raw text of the block the user tapped. */
  onText: (text: string) => void;
  disabled?: boolean;
  /**
   * Optional filter — hide detections that don't match. Useful for fields
   * with a known shape (e.g. `/^\d{8}$/` for a Bambu access code).
   */
  accept?: (text: string) => boolean;
  /**
   * Optional normaliser applied to each detection before display + onText.
   * Defaults to trimming whitespace.
   */
  normalize?: (text: string) => string;
}

/**
 * Inline scan button. Renders nothing when the browser lacks TextDetector,
 * so callers can drop it next to any input field without special-casing.
 */
export function TextScanner({ label = 'Scan', onText, disabled, accept, normalize }: TextScannerProps) {
  const [open, setOpen] = useState(false);
  if (!isTextDetectorSupported()) return null;

  return (
    <>
      <Button variant="secondary" onClick={() => setOpen(true)} disabled={disabled}>
        <ScanText size={14} /> {label}
      </Button>
      {open && (
        <TextScannerDialog
          onPick={(t) => { onText(t); setOpen(false); }}
          onClose={() => setOpen(false)}
          accept={accept}
          normalize={normalize}
        />
      )}
    </>
  );
}

interface DialogProps {
  onPick: (text: string) => void;
  onClose: () => void;
  accept?: (text: string) => boolean;
  normalize?: (text: string) => string;
}

function TextScannerDialog({ onPick, onClose, accept, normalize }: DialogProps) {
  const fileRef = useRef<HTMLInputElement>(null);
  const [imgUrl, setImgUrl] = useState<string | null>(null);
  const [detected, setDetected] = useState<string[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  // Trigger the file picker automatically on first open — matches how most
  // phones treat QR/barcode scan buttons. User can Retake from the dialog.
  useEffect(() => { fileRef.current?.click(); }, []);
  useEffect(() => () => { if (imgUrl) URL.revokeObjectURL(imgUrl); }, [imgUrl]);

  const norm = normalize ?? ((s) => s.trim());

  const handleFile = async (f: File | undefined) => {
    if (!f) return;
    setError(null);
    setDetected(null);
    const url = URL.createObjectURL(f);
    setImgUrl(url);
    try {
      const img = new Image();
      img.src = url;
      await img.decode();
      const bitmap = await createImageBitmap(img);
      const detector = new window.TextDetector!();
      const results = await detector.detect(bitmap);
      const cleaned = results
        .map((r) => norm(r.rawValue))
        .filter((t) => t.length > 0)
        .filter((t) => (accept ? accept(t) : true));
      setDetected(cleaned);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  return (
    <div
      className="fixed inset-0 z-50 bg-black/80 flex items-center justify-center p-4"
      onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}
    >
      <input
        ref={fileRef}
        type="file"
        accept="image/*"
        capture="environment"
        className="hidden"
        onChange={(e) => handleFile(e.target.files?.[0])}
      />
      <div className="bg-surface-card border border-surface-border rounded-card max-w-md w-full max-h-[90vh] overflow-auto p-4 shadow-xl">
        <div className="flex items-center justify-between mb-3">
          <h3 className="text-sm font-medium text-text-primary">Scan value</h3>
          <button onClick={onClose} className="text-text-muted hover:text-text-primary cursor-pointer" aria-label="Close">
            <X size={16} />
          </button>
        </div>

        {imgUrl ? (
          <img src={imgUrl} alt="" className="max-h-48 w-full object-contain bg-black rounded mb-3" />
        ) : (
          <div className="text-sm text-text-muted py-8 text-center">Opening camera…</div>
        )}

        {error && <div className="text-xs text-status-error mb-2">{error}</div>}

        {detected && detected.length === 0 && (
          <div className="text-sm text-text-muted">
            No text detected.{' '}
            <button onClick={() => fileRef.current?.click()} className="text-brand-400 underline cursor-pointer">
              Retake
            </button>
          </div>
        )}

        {detected && detected.length > 0 && (
          <>
            <div className="text-xs text-text-muted mb-2">Tap the value you want.</div>
            <div className="flex flex-wrap gap-2 mb-3">
              {detected.map((t, i) => (
                <button
                  key={`${i}-${t}`}
                  onClick={() => onPick(t)}
                  className="px-3 py-1.5 text-sm font-mono bg-surface-input border border-surface-border rounded text-text-primary hover:border-brand-500 hover:text-brand-400 transition-colors cursor-pointer break-all text-left"
                >
                  {t}
                </button>
              ))}
            </div>
            <Button variant="secondary" onClick={() => fileRef.current?.click()}>Retake</Button>
          </>
        )}
      </div>
    </div>
  );
}
