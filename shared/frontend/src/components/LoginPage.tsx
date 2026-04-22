import { useState } from 'react';
import { KeyRound } from 'lucide-react';
import { PasswordField } from './PasswordField';
import { Button } from './Button';

interface LoginPageProps {
  deviceName?: string;
  /** Called with the entered key + the user's "remember this device" choice. */
  onAuthenticated: (key: string, remember: boolean) => void;
}

/**
 * Full-screen sign-in view. Takes a security key, verifies it by calling
 * /api/auth-status with Authorization: Bearer <key>, and on success passes
 * the key to the parent which is responsible for storing it.
 */
export function LoginPage({ deviceName, onAuthenticated }: LoginPageProps) {
  const [key, setKey] = useState('');
  const [remember, setRemember] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async () => {
    if (!key) return;
    setBusy(true);
    setError(null);
    try {
      const r = await fetch('/api/auth-status', {
        headers: { Authorization: `Bearer ${key}` },
      });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const body = await r.json();
      if (body?.authenticated) {
        onAuthenticated(key, remember);
      } else {
        setError('Wrong key.');
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="min-h-screen bg-surface-body flex items-center justify-center p-4 font-sans text-text-primary">
      <div className="animate-in bg-surface-card border border-surface-border rounded-card p-6 max-w-sm w-full shadow-[0_10px_40px_rgba(0,0,0,0.3)]">
        <div className="flex items-center gap-2 mb-1">
          <KeyRound size={18} className="text-brand-400" />
          <h1 className="text-lg font-semibold text-text-primary">Locked</h1>
        </div>
        <div className="text-xs text-text-muted mb-5">
          {deviceName ? <span className="font-mono">{deviceName}</span> : 'This device'}{' '}
          needs the shared security key.
        </div>

        <div className="space-y-3">
          <PasswordField
            label="Security key"
            value={key}
            onChange={(e) => setKey(e.target.value)}
            autoFocus
            onKeyDown={(e) => { if (e.key === 'Enter') submit(); }}
          />
          <label className="flex items-center gap-2 text-sm text-text-secondary cursor-pointer">
            <input
              type="checkbox"
              checked={remember}
              onChange={(e) => setRemember(e.target.checked)}
              className="rounded border-surface-border accent-brand-500"
            />
            Remember this device
          </label>
          {error && <div className="text-xs text-status-error">{error}</div>}
          <Button onClick={submit} disabled={!key || busy}>
            {busy ? 'Checking…' : 'Sign in'}
          </Button>
        </div>
      </div>
    </div>
  );
}
