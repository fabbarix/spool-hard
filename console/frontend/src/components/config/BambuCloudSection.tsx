import { useState } from 'react';
import { Cloud, Copy, Check, RefreshCw, Trash2, KeyRound, AlertCircle } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';
import {
  useBambuCloud, useBambuLoginPassword, useBambuLoginCode,
  useBambuLoginTfa, useBambuSetToken, useBambuVerify, useBambuClear,
  type BambuRegion, type BambuLoginStepResult, type BambuStepDiagnostics,
} from '../../hooks/useBambuCloud';

// Two-step state machine on the React side. The backend is stateless;
// this tracks where we are in the multi-prompt login flow so the right
// follow-up field renders.
type LoginPhase =
  | { kind: 'idle' }
  | { kind: 'need_email_code' }
  | { kind: 'need_tfa'; tfa_key: string };

function fmtExpiry(unixTs: number | undefined): { label: string; tone: 'ok' | 'warn' | 'expired' | 'unknown' } {
  if (!unixTs) return { label: 'unknown', tone: 'unknown' };
  const ms = unixTs * 1000;
  const now = Date.now();
  const days = Math.floor((ms - now) / (24 * 60 * 60 * 1000));
  const date = new Date(ms);
  const dateStr = date.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
  if (ms < now)        return { label: `expired (${dateStr})`, tone: 'expired' };
  if (days < 14)       return { label: `${dateStr} (${days}d left)`, tone: 'warn' };
  return { label: `${dateStr} (${days}d left)`, tone: 'ok' };
}

export function BambuCloudSection() {
  const status   = useBambuCloud();
  const login    = useBambuLoginPassword();
  const loginCode = useBambuLoginCode();
  const loginTfa = useBambuLoginTfa();
  const setToken = useBambuSetToken();
  const verify   = useBambuVerify();
  const clear    = useBambuClear();

  // Form state (controlled inputs).
  const [account, setAccount]     = useState('');
  const [password, setPassword]   = useState('');
  const [region, setRegion]       = useState<BambuRegion>('global');
  const [emailCode, setEmailCode] = useState('');
  const [tfaCode, setTfaCode]     = useState('');
  const [phase, setPhase]         = useState<LoginPhase>({ kind: 'idle' });
  const [feedback, setFeedback]   = useState<{ msg: string; tone: 'ok' | 'err' | 'info' | 'warn' } | null>(null);
  // Last failed step's raw response details, kept until the next attempt
  // so the user can read it after the friendly error.
  const [lastDiagnostics, setLastDiagnostics] = useState<BambuStepDiagnostics | null>(null);

  // Manual token entry.
  const [manualToken, setManualToken] = useState('');
  const [manualEmail, setManualEmail] = useState('');

  // "Show / copy" UI for the existing token.
  const [revealToken, setRevealToken] = useState(false);
  const [copied, setCopied]           = useState(false);

  const handleStepResult = (res: BambuLoginStepResult) => {
    // The backend ships diagnostics on every non-Ok outcome. Surface
    // them on errors only — for the multi-step branches the friendly
    // message is enough.
    setLastDiagnostics(
      res.status === 'invalid_credentials' || res.status === 'network_error' || res.status === 'server_error'
        ? res.diagnostics ?? null
        : null,
    );
    if (res.status === 'ok') {
      setPhase({ kind: 'idle' });
      setPassword(''); setEmailCode(''); setTfaCode('');
      setFeedback({ msg: 'Logged in and saved.', tone: 'ok' });
    } else if (res.status === 'need_email_code') {
      setPhase({ kind: 'need_email_code' });
      setFeedback({ msg: res.message || 'Enter the code from your email.', tone: 'info' });
    } else if (res.status === 'need_tfa') {
      setPhase({ kind: 'need_tfa', tfa_key: res.tfa_key || '' });
      setFeedback({ msg: 'Enter your two-factor code.', tone: 'info' });
    } else {
      setFeedback({ msg: res.message || res.status, tone: 'err' });
    }
  };

  const submitPassword = () => {
    if (!account || !password) return;
    setFeedback(null); setLastDiagnostics(null);
    login.mutate({ account, password, region }, { onSuccess: handleStepResult });
  };

  const submitEmailCode = () => {
    if (!emailCode) return;
    setLastDiagnostics(null);
    loginCode.mutate({ account, code: emailCode, region }, { onSuccess: handleStepResult });
  };

  const submitTfa = () => {
    if (phase.kind !== 'need_tfa' || !tfaCode) return;
    setLastDiagnostics(null);
    loginTfa.mutate({ tfa_key: phase.tfa_key, tfa_code: tfaCode, account, region }, { onSuccess: handleStepResult });
  };

  const submitManualToken = () => {
    if (!manualToken) return;
    setFeedback(null); setLastDiagnostics(null);
    setToken.mutate(
      { token: manualToken.trim(), email: manualEmail, region },
      {
        onSuccess: (res) => {
          if (res.status === 'ok') {
            setManualToken(''); setManualEmail('');
            // verified=false: token was saved but the firmware couldn't
            // reach Bambu Cloud to confirm it (typically the same WAF
            // wall that blocks login). Token might still work for MQTT
            // etc. when those endpoints aren't blocked, but cloud
            // discovery / AMS reads will fail. Mark as warn, not ok.
            if (res.verified === false) {
              setFeedback({
                msg: res.message || 'Token saved, but couldn\'t reach Bambu Cloud to verify it. Cloud-dependent features may not work until network access to api.bambulab.com is possible from this device.',
                tone: 'warn',
              });
            } else {
              setFeedback({ msg: 'Token verified and saved.', tone: 'ok' });
            }
          } else {
            setFeedback({ msg: res.message || res.status, tone: 'err' });
          }
        },
      },
    );
  };

  const copyToClipboard = async (text: string) => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      setTimeout(() => setCopied(false), 1200);
    } catch {
      // Fallback for non-secure contexts (rare on real device + LAN HTTPS).
      window.prompt('Copy the token below:', text);
    }
  };

  const expiry = fmtExpiry(status.data?.expires_at);

  // ── Existing-token panel ──
  const tokenPanel = status.data?.configured ? (
    <div className="rounded-md border border-border bg-input p-4 space-y-3">
      <div className="flex items-center justify-between">
        <div>
          <div className="text-sm text-text-secondary">Currently authenticated as</div>
          <div className="font-medium text-text">{status.data.email || '(unknown account)'}</div>
        </div>
        <div className="text-xs font-mono text-text-secondary uppercase">
          {status.data.region}
        </div>
      </div>
      <div className="text-sm">
        <span className="text-text-secondary">Expires:</span>{' '}
        <span className={
          expiry.tone === 'expired' ? 'text-red-400' :
          expiry.tone === 'warn'    ? 'text-amber-400' :
          expiry.tone === 'ok'      ? 'text-teal-400'  :
                                      'text-text-secondary'
        }>{expiry.label}</span>
      </div>
      <div className="font-mono text-xs break-all bg-body rounded p-2 text-text-secondary">
        {revealToken ? status.data.token : status.data.token_preview}
      </div>
      <div className="flex flex-wrap gap-2">
        <Button variant="secondary" onClick={() => setRevealToken((v) => !v)}>
          {revealToken ? 'Hide' : 'Show'}
        </Button>
        <Button
          variant="secondary"
          onClick={() => status.data?.token && copyToClipboard(status.data.token)}
        >
          {copied ? <><Check size={14} className="inline mr-1" />Copied</> : <><Copy size={14} className="inline mr-1" />Copy</>}
        </Button>
        <Button
          variant="secondary"
          onClick={() => verify.mutate()}
          disabled={verify.isPending}
        >
          <RefreshCw size={14} className="inline mr-1" />
          {verify.isPending ? 'Verifying…' : 'Verify'}
        </Button>
        <Button
          variant="danger"
          onClick={() => clear.mutate()}
          disabled={clear.isPending}
        >
          <Trash2 size={14} className="inline mr-1" />
          Clear
        </Button>
      </div>
      {verify.data && (
        <div className={`text-sm ${verify.data.valid ? 'text-teal-400' : 'text-red-400'}`}>
          {verify.data.valid ? 'Token still valid.' : 'Server rejected the token — re-authenticate.'}
        </div>
      )}
    </div>
  ) : null;

  // ── Login form ──
  return (
    <SectionCard
      title="Bambu Lab Cloud"
      icon={<Cloud size={16} />}
      description="Sign in to fetch the user filament profiles you've configured in Bambu Studio / OrcaSlicer. The console stores the access token in NVS and uses it for cloud-API calls. You can also paste a token directly if you already have one."
    >
      {!status.data?.configured && (
        <div className="rounded-md border border-amber-700/40 bg-amber-950/20 p-3 text-xs text-amber-200">
          <div className="font-medium mb-1 flex items-center gap-1">
            <AlertCircle size={12} /> Heads up
          </div>
          Direct sign-in below sometimes fails with HTTP 403 — Cloudflare's bot
          detection in front of the Bambu API can flag the device's TLS
          fingerprint regardless of the credentials. If that happens, use
          <strong> "I already have a token" </strong> below to paste a token
          obtained from Bambu Studio / OrcaSlicer
          (<code className="font-mono">~/.bambu_token</code> on Linux/macOS).
          Token verification through the same API usually works once a token
          is in hand.
        </div>
      )}

      {tokenPanel}

      <div className="rounded-md border border-border bg-card p-4 space-y-3 mt-3">
        <div className="text-sm font-medium text-text">
          {status.data?.configured ? 'Re-authenticate' : 'Sign in'}
        </div>

        <div className="grid grid-cols-1 sm:grid-cols-2 gap-2">
          <InputField label="Email" value={account} onChange={(e) => setAccount(e.target.value)} autoComplete="username" />
          <div>
            <label className="block text-sm text-text-secondary mb-1">Region</label>
            <select
              value={region}
              onChange={(e) => setRegion(e.target.value as BambuRegion)}
              className="w-full rounded-md border border-border bg-input px-3 py-2 text-text focus:border-brand-500 outline-none"
            >
              <option value="global">Global (api.bambulab.com)</option>
              <option value="china">China (api.bambulab.cn)</option>
            </select>
          </div>
        </div>

        <InputField
          label="Password"
          type="password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          autoComplete="current-password"
        />

        {phase.kind === 'need_email_code' && (
          <InputField
            label="Email verification code"
            value={emailCode}
            onChange={(e) => setEmailCode(e.target.value)}
            autoComplete="one-time-code"
          />
        )}
        {phase.kind === 'need_tfa' && (
          <InputField
            label="Two-factor code"
            value={tfaCode}
            onChange={(e) => setTfaCode(e.target.value)}
            autoComplete="one-time-code"
          />
        )}

        <div className="flex gap-2 flex-wrap">
          {phase.kind === 'idle' && (
            <Button onClick={submitPassword} disabled={login.isPending || !account || !password}>
              {login.isPending ? 'Signing in…' : 'Sign in'}
            </Button>
          )}
          {phase.kind === 'need_email_code' && (
            <Button onClick={submitEmailCode} disabled={loginCode.isPending || !emailCode}>
              {loginCode.isPending ? 'Verifying…' : 'Verify code'}
            </Button>
          )}
          {phase.kind === 'need_tfa' && (
            <Button onClick={submitTfa} disabled={loginTfa.isPending || !tfaCode}>
              {loginTfa.isPending ? 'Verifying…' : 'Submit 2FA'}
            </Button>
          )}
          {phase.kind !== 'idle' && (
            <Button variant="secondary" onClick={() => { setPhase({ kind: 'idle' }); setFeedback(null); }}>
              Cancel
            </Button>
          )}
        </div>

        {feedback && (feedback.tone === 'warn' ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-sm text-amber-300">
            {feedback.msg}
          </div>
        ) : (
          <div className={
            feedback.tone === 'ok'   ? 'text-sm text-teal-400'  :
            feedback.tone === 'err'  ? 'text-sm text-red-400'   :
                                       'text-sm text-text-secondary'
          }>
            {feedback.msg}
          </div>
        ))}

        {lastDiagnostics && (
          <details className="rounded-md border border-red-700/40 bg-red-950/20 text-xs">
            <summary className="cursor-pointer p-2 text-red-300 flex items-center gap-2">
              <AlertCircle size={12} />
              Show raw response
            </summary>
            <div className="border-t border-red-700/40 p-3 space-y-2 font-mono">
              <div>
                <div className="text-text-secondary">Request</div>
                <div className="break-all">POST {lastDiagnostics.request_url}</div>
              </div>
              <div>
                <div className="text-text-secondary">HTTP status</div>
                <div>{lastDiagnostics.http_status || '(no response)'}</div>
              </div>
              {lastDiagnostics.response_headers && (
                <div>
                  <div className="text-text-secondary">Response headers</div>
                  <pre className="whitespace-pre-wrap break-all">{lastDiagnostics.response_headers}</pre>
                </div>
              )}
              <div>
                <div className="text-text-secondary">Response body</div>
                <pre className="whitespace-pre-wrap break-all bg-body rounded p-2 max-h-60 overflow-auto">
                  {lastDiagnostics.response_body || '(empty)'}
                </pre>
              </div>
              <div>
                <Button
                  variant="secondary"
                  onClick={() => copyToClipboard(JSON.stringify(lastDiagnostics, null, 2))}
                >
                  <Copy size={12} className="inline mr-1" />
                  Copy details
                </Button>
              </div>
            </div>
          </details>
        )}
      </div>

      {/* ── Direct token entry ── */}
      <details className="rounded-md border border-border bg-card p-4 mt-3 group">
        <summary className="cursor-pointer text-sm font-medium text-text flex items-center gap-2">
          <KeyRound size={14} />
          I already have a token
        </summary>
        <div className="mt-3 space-y-2">
          <p className="text-xs text-text-secondary">
            Paste either:
            {' '}<strong>(a)</strong> a raw access-token JWT (from OrcaSlicer's
            <code className="font-mono"> ~/.bambu_token</code>, Bambu Studio log,
            or a previous run of this device), or
            {' '}<strong>(b)</strong> the <code className="font-mono">SPOOLHARD-TOKEN:</code>
            blob produced by{' '}
            <a
              className="underline text-brand-500"
              href="https://github.com/fabbarix/spool-hard/blob/main/tools/bambu_login.py"
              target="_blank" rel="noopener noreferrer"
            >tools/bambu_login.py</a>{' '}
            — that script does the Bambu login on a real computer (where
            Cloudflare's WAF lets the request through) and packages
            token + region + account into one line.
          </p>
          <InputField
            label="Token (or SPOOLHARD-TOKEN: blob)"
            value={manualToken}
            onChange={(e) => setManualToken(e.target.value)}
          />
          {manualToken.trimStart().startsWith('SPOOLHARD-TOKEN:') ? (
            <div className="text-xs text-text-secondary italic">
              Paste-blob detected — region and account label will come from
              the blob.
            </div>
          ) : (
            <InputField
              label="Account label (optional, just for the UI)"
              value={manualEmail}
              onChange={(e) => setManualEmail(e.target.value)}
            />
          )}
          <Button onClick={submitManualToken} disabled={setToken.isPending || !manualToken}>
            {setToken.isPending ? 'Verifying…' : 'Verify & save'}
          </Button>
        </div>
      </details>
    </SectionCard>
  );
}
