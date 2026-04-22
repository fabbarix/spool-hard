import { createContext, useCallback, useContext, useRef, useState, type ReactNode } from 'react';

export type ReconnectPhase = 'idle' | 'waiting' | 'probing' | 'back' | 'timeout';

interface ReconnectContextValue {
  phase: ReconnectPhase;
  message: string;
  /**
   * Trigger the reconnect watcher. Call this AFTER an action that will cause
   * the device to restart (firmware/frontend upload completed, Restart or
   * Factory-Reset buttons). Safe to call repeatedly — subsequent calls while
   * one is in progress just refresh the message.
   */
  start: (message?: string) => void;
}

const ReconnectContext = createContext<ReconnectContextValue | null>(null);

// Endpoint used as the heartbeat. /api/auth-status is the right choice:
//   * it exists on both scale and console,
//   * it never returns 401 (by design — it's the login probe),
//   * it's cheap and returns 200 with a tiny JSON body.
const PROBE_URL = '/api/auth-status';

// Wait this long after start() before the first probe — the device needs
// time to finish its response, actually reboot, and bring WiFi back up.
const INITIAL_DELAY_MS = 3000;

// Spacing between probes once polling begins. Short enough that the user
// doesn't sit waiting once the device is up, long enough to not flood the
// network.
const POLL_INTERVAL_MS = 1000;

// Consecutive successful probes required before we consider the device
// back online and reload the page. Two in a row guards against the rare
// case where the same socket gets a stale-cache 200.
const REQUIRED_SUCCESSES = 2;

// Hard cap — after this long we stop polling and show a "didn't come back"
// message so the user knows to check network / power.
const TIMEOUT_MS = 90_000;

export function ReconnectProvider({ children }: { children: ReactNode }) {
  const [phase, setPhase] = useState<ReconnectPhase>('idle');
  const [message, setMessage] = useState<string>('');
  // Hold the active attempt's cancellation token in a ref so a re-render
  // triggered by setPhase doesn't kick off a parallel poll loop.
  const attemptRef = useRef<number>(0);

  const start = useCallback((msg = 'Device is restarting') => {
    const myAttempt = ++attemptRef.current;
    setMessage(msg);
    setPhase('waiting');

    const done = () => attemptRef.current !== myAttempt;

    const poll = async () => {
      if (done()) return;
      let successes = 0;
      const startedAt = Date.now();

      while (!done()) {
        if (Date.now() - startedAt > TIMEOUT_MS) {
          if (!done()) {
            setPhase('timeout');
            setMessage('The device did not come back online. Check power and network, then refresh this page.');
          }
          return;
        }
        try {
          const res = await fetch(PROBE_URL, { cache: 'no-store' });
          if (res.ok) {
            successes++;
            if (successes >= REQUIRED_SUCCESSES) {
              if (!done()) {
                setPhase('back');
                // Brief delay so the user registers the green "back online"
                // state before the page reloads.
                setTimeout(() => { if (!done()) window.location.reload(); }, 600);
              }
              return;
            }
          } else {
            successes = 0;
          }
        } catch {
          successes = 0;
        }
        if (!done()) await new Promise((r) => setTimeout(r, POLL_INTERVAL_MS));
      }
    };

    // Wait INITIAL_DELAY_MS before the first probe so we're not hammering
    // the device while it's still in its "response sent, about to reboot"
    // window. setPhase('probing') lets the overlay show "Reconnecting…"
    // during this window.
    setTimeout(() => {
      if (done()) return;
      setPhase('probing');
      void poll();
    }, INITIAL_DELAY_MS);
  }, []);

  return (
    <ReconnectContext.Provider value={{ phase, message, start }}>
      {children}
    </ReconnectContext.Provider>
  );
}

export function useReconnect() {
  const ctx = useContext(ReconnectContext);
  if (!ctx) throw new Error('useReconnect must be used inside <ReconnectProvider>');
  return ctx;
}
